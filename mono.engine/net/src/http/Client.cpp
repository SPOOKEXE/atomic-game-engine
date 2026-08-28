#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/net/http/Client.hpp>

#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

// The connecting socket, and the second of the two files in this module that
// knows what asio is.
//
// **Non-blocking connect, and the completion is detected by connecting again.**
// A non-blocking `connect` returns `in_progress` and finishes later, and asio's
// synchronous surface has no "is it done yet" call. Re-issuing it is the
// portable answer: the operating system answers `already_connected` once the
// handshake landed, `already_started` while it is still going, and the real
// failure - refused, unreachable - once it has one. The alternative,
// `remote_endpoint`, cannot tell "still connecting" from "refused", so a
// refused connection would sit there until an idle bound expired instead of
// failing immediately.
//
// **One socket per fetch.** Groups stream concurrently so one slow group does
// not hold up the others; sharing a socket would put them back in one queue,
// which is the head-of-line blocking that arrangement exists to remove.

namespace engine::net::http {
	namespace {
		constexpr int MAXIMUM_IO_PER_PUMP = 16;
		constexpr size_t READ_CHUNK_BYTES = 64u * 1024u;

		std::optional<asio::ip::tcp::endpoint> ToAsio(const Endpoint &source) {
			if (source.Family == AddressFamily::IPv4) {
				asio::ip::address_v4::bytes_type bytes{};
				std::memcpy(bytes.data(), source.Address.data(), bytes.size());
				return asio::ip::tcp::endpoint(asio::ip::address_v4(bytes), source.Port);
			}
			if (source.Family == AddressFamily::IPv6) {
				asio::ip::address_v6::bytes_type bytes{};
				std::memcpy(bytes.data(), source.Address.data(), bytes.size());
				return asio::ip::tcp::endpoint(asio::ip::address_v6(bytes), source.Port);
			}
			return std::nullopt;
		}

		struct Fetch {
			explicit Fetch(asio::io_context &context) : Socket(context) {}

			asio::ip::tcp::socket Socket;
			asio::ip::tcp::endpoint Target;

			std::vector<std::byte> Outbox;
			size_t Sent = 0;
			std::vector<std::byte> Inbox;

			FetchState State = FetchState::Pending;
			bool Connected = false;

			// Whether the request was a `Head`. Kept because a response to one
			// carries a length and no body, and the reader cannot tell from the
			// bytes - see `ParseResponse`.
			bool BodyOmitted = false;

			Response Answer;
			uint32_t Quiet = 0;
		};

		class TcpClient final : public Client {
		  public:
			explicit TcpClient(const ClientSettings &settings) : Limits(settings) {}

			~TcpClient() override {
				for (auto &entry : Fetches) {
					std::error_code ignored;
					entry.second->Socket.close(ignored);
				}
			}

			FetchId Submit(const Endpoint &to, const Request &request, std::string_view host) override {
				if (Outstanding() >= Limits.MaximumOutstanding) {
					// Refused rather than queued: a queue here would hide that
					// the caller is asking for more than it configured for, and
					// the symptom would be latency nobody can attribute.
					return {};
				}
				const std::optional<asio::ip::tcp::endpoint> target = ToAsio(to);
				if (!target) {
					return {};
				}

				auto fetch = std::make_unique<Fetch>(Context);
				fetch->Target = *target;
				fetch->BodyOmitted = request.Verb == Method::Head;
				WriteRequest(request, host, fetch->Outbox);

				std::error_code failure;
				fetch->Socket.open(fetch->Target.protocol(), failure);
				if (failure) {
					return {};
				}
				fetch->Socket.non_blocking(true, failure);
				if (failure) {
					std::error_code ignored;
					fetch->Socket.close(ignored);
					return {};
				}
				fetch->Socket.set_option(asio::ip::tcp::no_delay(true), failure);

				const FetchId id{.Value = NextFetch++};
				Fetches.emplace_back(id.Value, std::move(fetch));
				return id;
			}

			FetchState StateOf(FetchId id) const override {
				const Fetch *const fetch = Find(id);
				return fetch ? fetch->State : FetchState::Unknown;
			}

			size_t Pump() override {
				size_t finished = 0;
				for (auto &entry : Fetches) {
					Fetch &fetch = *entry.second;
					if (fetch.State != FetchState::Pending) {
						continue;
					}

					const size_t before = fetch.Sent + fetch.Inbox.size();
					Drive(fetch);

					if (fetch.State != FetchState::Pending) {
						++finished;
						continue;
					}
					if (Limits.IdlePolls > 0) {
						const size_t after = fetch.Sent + fetch.Inbox.size();
						fetch.Quiet = (after == before) ? fetch.Quiet + 1 : 0;
						if (fetch.Quiet >= Limits.IdlePolls) {
							Fail(fetch, "nothing moved for the idle poll budget");
							++finished;
						}
					}
				}
				return finished;
			}

			std::optional<Response> Take(FetchId id) override {
				for (auto entry = Fetches.begin(); entry != Fetches.end(); ++entry) {
					if (entry->first != id.Value) {
						continue;
					}
					if (entry->second->State != FetchState::Ready) {
						return std::nullopt;
					}
					Response answer = std::move(entry->second->Answer);
					std::error_code ignored;
					entry->second->Socket.close(ignored);
					Fetches.erase(entry);
					return answer;
				}
				return std::nullopt;
			}

			bool Cancel(FetchId id) override {
				for (auto entry = Fetches.begin(); entry != Fetches.end(); ++entry) {
					if (entry->first != id.Value) {
						continue;
					}
					const FetchState state = entry->second->State;
					std::error_code ignored;
					entry->second->Socket.close(ignored);
					Fetches.erase(entry);
					return state == FetchState::Pending;
				}
				return false;
			}

			size_t Outstanding() const override {
				return Fetches.size();
			}

			uint64_t ReceivedBytes() const override {
				return Received;
			}

		  private:
			const Fetch *Find(FetchId id) const {
				for (const auto &entry : Fetches) {
					if (entry.first == id.Value) {
						return entry.second.get();
					}
				}
				return nullptr;
			}

			// **Every way a fetch dies passes through here, and each says which
			// one it was.** A refused connect, a truncated response and a
			// malformed one are three different problems that all surfaced as
			// `FetchState::Failed` with nothing else recorded.
			void Fail(Fetch &fetch, const char *why) {
				fetch.State = FetchState::Failed;
				std::error_code ignored;
				fetch.Socket.close(ignored);
				core::Metrics::Count("net.http.fetch.failed", 1.0);
				ENGINE_WARN("fetch of {} failed: {}", fetch.Target.address().to_string(), why);
			}

			void Drive(Fetch &fetch) {
				if (!fetch.Connected && !Connect(fetch)) {
					return;
				}
				if (fetch.Sent < fetch.Outbox.size() && !Send(fetch)) {
					return;
				}
				if (fetch.Sent >= fetch.Outbox.size()) {
					Receive(fetch);
				}
			}

			// @return Whether the connection is up. A `false` with the state
			//         still `Pending` means "not yet"; check the state to tell
			//         that from a failure.
			bool Connect(Fetch &fetch) {
				std::error_code failure;
				fetch.Socket.connect(fetch.Target, failure);

				if (!failure || failure == asio::error::already_connected) {
					fetch.Connected = true;
					return true;
				}
				if (failure == asio::error::in_progress || failure == asio::error::would_block ||
					failure == asio::error::try_again || failure == asio::error::already_started) {
					return false;
				}
				// Refused, unreachable, or the host went away. One state for
				// all of them: a caller retries or falls through to the next
				// source either way.
				Fail(fetch, failure.message().c_str());
				return false;
			}

			bool Send(Fetch &fetch) {
				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					if (fetch.Sent >= fetch.Outbox.size()) {
						return true;
					}
					std::error_code failure;
					const size_t written = fetch.Socket.write_some(
						asio::buffer(fetch.Outbox.data() + fetch.Sent, fetch.Outbox.size() - fetch.Sent),
						failure
					);
					fetch.Sent += written;

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						return false;
					}
					if (failure) {
						Fail(fetch, "the socket refused the request bytes");
						return false;
					}
				}
				return fetch.Sent >= fetch.Outbox.size();
			}

			void Receive(Fetch &fetch) {
				// Whether the peer finished the conversation during this poll.
				// **Load-bearing**: a connection that closes with an incomplete
				// message must fail rather than stay pending, or an origin that
				// refuses by hanging up leaves the caller waiting on a socket
				// that is already gone. That is exactly what a connection
				// ceiling looks like from this end.
				bool finished = false;

				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					const size_t offset = fetch.Inbox.size();
					// The transfer bound. What the *content* is allowed to
					// weigh is the signed manifest's business and is checked a
					// layer up; this is the socket refusing to buffer forever.
					const uint64_t ceiling =
						Limits.Limits.BodyBytes + Limits.Limits.HeaderBytes + Limits.Limits.RequestLineBytes;
					if (offset >= ceiling) {
						Fail(fetch, "the response is past the transfer ceiling");
						return;
					}

					fetch.Inbox.resize(offset + READ_CHUNK_BYTES);
					std::error_code failure;
					const size_t received = fetch.Socket.read_some(
						asio::buffer(fetch.Inbox.data() + offset, READ_CHUNK_BYTES), failure
					);
					fetch.Inbox.resize(offset + received);
					Received += received;

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						break;
					}
					if (failure) {
						// eof with a complete message already buffered is fine
						// - the parse below decides. eof with a partial one is
						// a truncated response, which must never read as a
						// complete short body.
						finished = true;
						break;
					}
					if (received == 0) {
						finished = true;
						break;
					}
				}

				Response answer;
				size_t consumed = 0;
				const ParseResult parsed =
					ParseResponse(fetch.Inbox, Limits.Limits, fetch.BodyOmitted, answer, consumed);
				if (parsed == ParseResult::Ok) {
					fetch.Answer = std::move(answer);
					fetch.State = FetchState::Ready;
					return;
				}
				if (parsed != ParseResult::Incomplete || finished) {
					// `Incomplete` here means the peer hung up mid-message,
					// which must never read as a complete short body.
					Fail(
						fetch,
						parsed == ParseResult::Incomplete ? "the peer closed before the message was whole"
														  : Describe(parsed)
					);
				}
			}

			ClientSettings Limits;

			// Declared before the sockets, which are constructed against it and
			// destroyed before it.
			asio::io_context Context;

			// A vector of pairs rather than a map: an outstanding set bounded
			// at sixteen is walked faster than it is hashed, and `cdn::Origin`
			// keeps its request table the same way.
			std::vector<std::pair<uint64_t, std::unique_ptr<Fetch>>> Fetches;
			uint64_t NextFetch = 1;
			uint64_t Received = 0;
		};
	}

	const char *Describe(FetchState state) {
		switch (state) {
		case FetchState::Unknown:
			return "unknown";
		case FetchState::Pending:
			return "pending";
		case FetchState::Ready:
			return "ready";
		case FetchState::Cancelled:
			return "cancelled";
		case FetchState::Failed:
			return "failed";
		}
		return "unknown";
	}

	std::unique_ptr<Client> MakeClient(const ClientSettings &settings) {
		return std::make_unique<TcpClient>(settings);
	}
}
