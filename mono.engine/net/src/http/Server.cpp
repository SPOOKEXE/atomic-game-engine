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

// The listening socket, and one of the two files in this module that knows what
// asio is.
//
// **Synchronous and non-blocking, exactly as `UdpTransport` is, and for a
// related reason.** That file wants datagrams to arrive on the tick's thread; a
// server has no tick but does have a handler that runs user code, and a handler
// firing on an asio reactor thread would put every caller of this class into a
// conversation about thread safety it should not have to have. So sockets are
// non-blocking, `io_context` exists only because a socket has to be constructed
// against one, and nothing here ever calls `run()` or `poll()`.
//
// **Every failure is a status, never an exception.** A peer that vanishes
// mid-response is an ordinary event on a public port.
//
// **A connection is dropped whole, never half-parsed.** Once a message's
// framing is in doubt there is nowhere to resume from: finding "the next
// message" means guessing, and a parser that guesses is the desync that request
// smuggling is built out of.

namespace engine::net::http {
	namespace {
		// How many read or write system calls one connection gets per pump.
		// Bounded so one fast peer cannot hold the pump while others wait.
		constexpr int MAXIMUM_IO_PER_PUMP = 16;

		// How much is asked for per read call.
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

		// One accepted peer, and everything owed to it.
		struct Connection {
			explicit Connection(asio::ip::tcp::socket socket) : Socket(std::move(socket)) {}

			asio::ip::tcp::socket Socket;
			std::vector<std::byte> Inbox;
			std::vector<std::byte> Outbox;

			// How much of Outbox has actually left. A partial write is the
			// normal case on a large body — the kernel's send buffer is smaller
			// than a compressed group — so the remainder is kept rather than
			// retried from the start.
			size_t Sent = 0;

			// Set once the response is queued and the peer asked not to keep
			// the connection. The socket stays open until Outbox has drained,
			// or the last bytes of the answer are lost.
			bool CloseWhenDrained = false;

			// Polls since anything moved, for the idle bound.
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
				// Without this a restarted origin cannot rebind its own port
				// until the kernel's TIME_WAIT expires, which reads as "the
				// port is taken" for a minute after a deploy.
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
				// Cached at bind: a port of zero becomes a real one exactly
				// once, and local_endpoint is a system call.
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
			// Takes whatever the backlog is holding, up to the ceiling.
			void Accept(ServeReport &report) {
				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					std::error_code failure;
					asio::ip::tcp::socket socket(Context);
					Acceptor.accept(socket, failure);
					if (failure) {
						// would_block is the ordinary "nothing waiting".
						return;
					}

					if (Live.size() >= Limits.MaximumConnections) {
						// Accepted and closed rather than left in the backlog:
						// a client waiting for a timeout is worse off than one
						// told immediately, and the kernel's queue is not a
						// place to store a refusal.
						std::error_code ignored;
						socket.close(ignored);
						++report.Rejected;
						continue;
					}

					std::error_code sizing;
					socket.non_blocking(true, sizing);
					// Nagle would hold a small response back waiting for more
					// to coalesce with, which on a request/response protocol is
					// latency bought with nothing.
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
				if (!Write(connection)) {
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
				// A connection with an answer still going out is not read from.
				// Reading the next request before this one has been written is
				// pipelining, and the queue it needs is a second place for the
				// framing to get out of step.
				if (connection.Sent < connection.Outbox.size()) {
					return true;
				}

				for (int attempt = 0; attempt < MAXIMUM_IO_PER_PUMP; ++attempt) {
					const size_t offset = connection.Inbox.size();
					if (offset >= Limits.ConnectionBufferBytes) {
						// A peer that sends header bytes forever without ever
						// finishing a message. Bounded here rather than by the
						// message limits, which bound one message rather than a
						// peer that never completes one.
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

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						return true;
					}
					if (failure) {
						// eof or a reset. Either way the peer is gone, and a
						// partial request it left behind is not a message.
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
					// Answered rather than dropped silently, then closed: a
					// client that sent something malformed learns which of the
					// two it was, and the connection ends because there is no
					// safe place to resume parsing from.
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
					// Well-formed and not something this subset does. Answered
					// rather than refused at the parser, so a client sending
					// POST gets a status instead of a dropped socket.
					answer.Code = Status::NotImplemented;
				} else {
					answer = handler ? handler(request) : Response{};
					if (answer.Code == Status::Unknown) {
						// A handler that fell through is a bug at this end.
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

			bool Write(Connection &connection) {
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

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						// The kernel's send buffer is full, which on a
						// multi-megabyte group is the normal case rather than
						// an error. The remainder goes out next pump.
						return true;
					}
					if (failure) {
						return false;
					}
				}
				return true;
			}

			ServerSettings Limits;

			// Declared before the acceptor and the sockets, which are
			// constructed against it and destroyed before it.
			asio::io_context Context;
			asio::ip::tcp::acceptor Acceptor;

			Endpoint Address;

			// unique_ptr rather than by value: a socket is not movable on every
			// standard library this builds against, and a connection's buffers
			// must not move under a partially completed write.
			std::vector<std::unique_ptr<Connection>> Live;
		};
	}

	std::unique_ptr<Server> Listen(uint16_t port, const ServerSettings &settings) {
		auto server = std::make_unique<TcpServer>(settings);
		if (!server->Bind(port)) {
			// A null rather than a throw: a port already in use is an ordinary
			// outcome of starting a second origin on one machine, and a caller
			// can pick another.
			return nullptr;
		}
		return server;
	}
}
