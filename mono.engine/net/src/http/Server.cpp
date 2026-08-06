#include <engine/net/http/Server.hpp>

#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <cstring>
#include <deque>
#include <memory>
#include <system_error>
#include <utility>

// Non-blocking, caller-thread-polled socket implementation.
//
// All socket work stays on the caller's thread. Malformed framing is never
// resumed from mid-buffer.

namespace engine::net::http {
	namespace {
		// Bounds per-connection work in one pump.
		constexpr int MAXIMUM_IO_PER_PUMP = 16;

		constexpr size_t READ_CHUNK_BYTES = 16u * 1024u;

		Endpoint FromAsio(const asio::ip::tcp::endpoint &source) {
			const asio::ip::address address = source.address();
			if (address.is_v4()) {
				return Endpoint::FromIPv4(address.to_v4().to_bytes(), source.port());
			}
			if (address.is_v6()) {
				return Endpoint::FromIPv6(address.to_v6().to_bytes(), source.port());
			}
			return {};
		}

		struct Connection {
			explicit Connection(asio::ip::tcp::socket socket) : Socket(std::move(socket)) {}

			asio::ip::tcp::socket Socket;
			std::vector<std::byte> Inbox;
			std::vector<std::byte> Outbox;

			// Bytes already written; the remainder is retried next pump.
			size_t Sent = 0;

			// Close after the queued response drains.
			bool CloseWhenDrained = false;

			uint32_t Quiet = 0;
		};

		class TcpServer final : public Server {
		  public:
			explicit TcpServer(const ServerSettings &settings) : Limits(settings), Acceptor(Context) {}

			~TcpServer() override {
				Close();
			}

			bool Bind(uint16_t port) {
				const asio::ip::tcp::endpoint local(asio::ip::tcp::v4(), port);

				std::error_code failure;
				Acceptor.open(local.protocol(), failure);
				if (failure) {
					return false;
				}
				// Allow rebinding during the kernel's TIME_WAIT period.
				Acceptor.set_option(asio::socket_base::reuse_address(true), failure);
				Acceptor.bind(local, failure);
				if (failure) {
					Acceptor.close(failure);
					return false;
				}
				Acceptor.listen(asio::socket_base::max_listen_connections, failure);
				if (failure) {
					Acceptor.close(failure);
					return false;
				}
				Acceptor.non_blocking(true, failure);
				if (failure) {
					Acceptor.close(failure);
					return false;
				}

				const asio::ip::tcp::endpoint bound = Acceptor.local_endpoint(failure);
				if (failure) {
					Acceptor.close(failure);
					return false;
				}
				Address = FromAsio(bound);
				return true;
			}

			ServeReport Pump(const Handler &handler) override {
				ServeReport report;
				if (!Open()) {
					return report;
				}

				Accept(report);

				size_t dispatched = 0;
				for (auto entry = Live.begin(); entry != Live.end();) {
					Connection &connection = **entry;
					const bool keep = Service(connection, handler, dispatched, report);
					if (keep) {
						++entry;
						continue;
					}
					std::error_code ignored;
					connection.Socket.close(ignored);
					entry = Live.erase(entry);
				}
				return report;
			}

			Endpoint Local() const override {
				return Acceptor.is_open() ? Address : Endpoint{};
			}

			size_t Connections() const override {
				return Live.size();
			}

			bool Open() const override {
				return Acceptor.is_open();
			}

			void Close() override {
				std::error_code ignored;
				for (const auto &connection : Live) {
					connection->Socket.close(ignored);
				}
				Live.clear();
				Acceptor.close(ignored);
			}

		  private:
			// Accepts ready connections up to the configured ceiling.
			void Accept(ServeReport &report) {
				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					std::error_code failure;
					asio::ip::tcp::socket socket(Context);
					Acceptor.accept(socket, failure);
					if (failure) {
						return;
					}

					if (Live.size() >= Limits.MaximumConnections) {
						// Reject immediately rather than leaving the client queued.
						std::error_code ignored;
						socket.close(ignored);
						++report.Rejected;
						continue;
					}

					std::error_code sizing;
					socket.non_blocking(true, sizing);
					socket.set_option(asio::ip::tcp::no_delay(true), sizing);

					Live.push_back(std::make_unique<Connection>(std::move(socket)));
					++report.Accepted;
				}
			}

			// Reads, parses, dispatches and writes one connection.
			//
			// @return Whether to keep it.
			bool
			Service(Connection &connection, const Handler &handler, size_t &dispatched, ServeReport &report) {
				const size_t before = connection.Inbox.size() + connection.Sent;

				if (!Read(connection, report)) {
					++report.Closed;
					return false;
				}
				if (!Dispatch(connection, handler, dispatched, report)) {
					return false;
				}
				if (!Write(connection, report)) {
					++report.Closed;
					return false;
				}

				if (connection.CloseWhenDrained && connection.Sent >= connection.Outbox.size()) {
					++report.Closed;
					return false;
				}

				if (Limits.IdlePolls > 0) {
					const size_t after = connection.Inbox.size() + connection.Sent;
					connection.Quiet = (after == before) ? connection.Quiet + 1 : 0;
					if (connection.Quiet >= Limits.IdlePolls) {
						++report.Closed;
						return false;
					}
				}
				return true;
			}

			bool Read(Connection &connection, ServeReport &report) {
				// Do not pipeline while the current response is pending.
				if (connection.Sent < connection.Outbox.size()) {
					return true;
				}

				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					const size_t offset = connection.Inbox.size();
					if (offset >= Limits.ConnectionBufferBytes) {
						// Bound peers that never finish a request.
						++report.Rejected;
						return false;
					}
					const size_t room = std::min(READ_CHUNK_BYTES, Limits.ConnectionBufferBytes - offset);
					connection.Inbox.resize(offset + room);

					std::error_code failure;
					const size_t received = connection.Socket.read_some(
						asio::buffer(connection.Inbox.data() + offset, room), failure
					);
					connection.Inbox.resize(offset + received);
					report.ReceivedBytes += received;

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						return true;
					}
					if (failure) {
						return false;
					}
					if (received == 0) {
						return true;
					}
				}
				return true;
			}

			bool Dispatch(
				Connection &connection, const Handler &handler, size_t &dispatched, ServeReport &report
			) {
				if (connection.Sent < connection.Outbox.size() || connection.Inbox.empty()) {
					return true;
				}
				if (dispatched >= Limits.DispatchPerPump) {
					return true;
				}

				Request request;
				size_t consumed = 0;
				const ParseResult parsed = ParseRequest(connection.Inbox, Limits.Limits, request, consumed);

				if (parsed == ParseResult::Incomplete) {
					return true;
				}
				if (parsed != ParseResult::Ok) {
					// Report the parse error, then close because framing cannot resume.
					Response refusal;
					refusal.Code =
						parsed == ParseResult::TooLarge ? Status::ContentTooLarge : Status::BadRequest;
					refusal.Set("connection", "close");
					Queue(connection, refusal, false);
					connection.CloseWhenDrained = true;
					++report.Rejected;
					return true;
				}

				connection.Inbox.erase(
					connection.Inbox.begin(), connection.Inbox.begin() + static_cast<ptrdiff_t>(consumed)
				);

				Response answer;
				if (request.Verb == Method::Unknown) {
					// Well-formed but unsupported methods receive 501.
					answer.Code = Status::NotImplemented;
				} else {
					answer = handler ? handler(request) : Response{};
					if (answer.Code == Status::Unknown) {
						answer = Response{};
						answer.Code = Status::InternalError;
					}
				}

				const bool asked = request.Find("connection") == "close";
				if (asked) {
					answer.Set("connection", "close");
				}
				Queue(connection, answer, request.Verb == Method::Head);
				connection.CloseWhenDrained = asked;

				++dispatched;
				++report.Served;
				return true;
			}

			static void Queue(Connection &connection, const Response &answer, bool bodyOmitted) {
				connection.Outbox.clear();
				connection.Sent = 0;
				WriteResponse(answer, bodyOmitted, connection.Outbox);
			}

			bool Write(Connection &connection, ServeReport &report) {
				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					if (connection.Sent >= connection.Outbox.size()) {
						return true;
					}
					std::error_code failure;
					const size_t written = connection.Socket.write_some(
						asio::buffer(
							connection.Outbox.data() + connection.Sent,
							connection.Outbox.size() - connection.Sent
						),
						failure
					);
					connection.Sent += written;
					report.SentBytes += written;

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						// Retry the unsent remainder next pump.
						return true;
					}
					if (failure) {
						return false;
					}
				}
				return true;
			}

			ServerSettings Limits;

			// Must outlive the acceptor and its sockets.
			asio::io_context Context;
			asio::ip::tcp::acceptor Acceptor;

			Endpoint Address;

			std::vector<std::unique_ptr<Connection>> Live;
		};
	}

	std::unique_ptr<Server> Listen(uint16_t port, const ServerSettings &settings) {
		auto server = std::make_unique<TcpServer>(settings);
		if (!server->Bind(port)) {
			// Binding failure is reported as null.
			return nullptr;
		}
		return server;
	}
}
