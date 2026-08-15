// The control socket, and the editor's frame loop that drives it.
//
// **Asynchronous asio with no thread of its own, polled from `Pump`.** The
// operations are all `async_`, but nothing here ever calls `io_context::run` -
// `Pump` calls `poll`, which runs whatever is ready and returns. So the socket
// makes progress one frame at a time, on the editor's thread, and there is no
// second thread and nothing that blocks.
//
// That is not a style preference, it is the bug class. A thread blocked in
// `accept` does not come out because the acceptor was closed, and a thread
// blocked in `read` does not come out because a flag was cleared - so an editor
// asked to quit joined threads that were waiting for a connection and a line
// that were never going to arrive, and the program hung with its window already
// gone. Quitting an editor started with `--mcp-port` did that every time, with
// no client ever attached. An operation that never blocks cannot be stuck, so
// `Stop` has nothing to wake and nothing to join.
//
// It also makes the one rule this has to obey free rather than engineered. Only
// the editor's thread may look at a world - `Universe::Enter` aborts on
// re-entry rather than racing - and a completion handler runs on whichever
// thread called `poll`, which is that thread by construction. The queue, the
// mutex and the condition variable that used to carry a request across the
// boundary are gone because there is no longer a boundary.
//
// The framing is one JSON object per line, which is MCP's own stdio framing.
// `mono.tools/mcpbridge` copies bytes between stdio and this socket without
// parsing them, so the two ends agree by construction.

#include <engine/control/Server.hpp>
#include <engine/core/Log.hpp>

#include <asio.hpp>
#include <istream>
#include <memory>
#include <string>
#include <utility>

namespace engine::control {

	struct Server::Impl {
		asio::io_context Context;
		std::unique_ptr<asio::ip::tcp::acceptor> Acceptor;

		// The one client, when there is one, and the buffers its exchange runs
		// through. Held rather than passed around because every continuation
		// below has to find them again.
		std::unique_ptr<asio::ip::tcp::socket> Client;
		asio::streambuf Incoming;

		// **The response outlives the call that wrote it.** `async_write` keeps
		// the buffer, not a copy of it, so a local string would be gone by the
		// time the write happened.
		std::string Outgoing;

		// Plain, not atomic: every one of these is touched only by the thread
		// that calls `Pump`, which is the editor's.
		bool Running = false;
		size_t Count = 0;
		uint16_t Bound = 0;

		// What answers a request, for exactly as long as one `Pump` lasts.
		//
		// The read continuation needs it and only ever runs inside `poll`, so
		// pointing at the caller's handler is sound and copying a `std::function`
		// per request is not needed.
		const Handler *Answering = nullptr;

		// Keeps one accept outstanding, always.
		//
		// **Re-armed even after a connection is refused**, because otherwise the
		// second client to be turned away would be the last one ever noticed.
		void Accept() {
			Acceptor->async_accept([this](const asio::error_code &failed, asio::ip::tcp::socket socket) {
				if (failed) {
					// The acceptor was closed by `Stop`. Nothing to re-arm.
					return;
				}

				asio::error_code ignored;

				// **Nagle off.** Every message here is one small line and the far
				// end is waiting for it; a 40ms coalescing delay on a
				// request/response protocol is the whole latency budget.
				socket.set_option(asio::ip::tcp::no_delay(true), ignored);

				if (Client != nullptr) {
					// **A second client is closed, and closing it is the point.**
					// One at a time is the contract; an extra connection left
					// established and unread is the worst of the three options,
					// because its end believes it is talking to an editor and
					// waits for an answer nothing is going to write. A close is
					// something it can act on.
					socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
					socket.close(ignored);
					ENGINE_INFO("control: refused a second client, one drives the editor at a time.");
				} else {
					Client = std::make_unique<asio::ip::tcp::socket>(std::move(socket));
					Incoming.consume(Incoming.size());
					Read();
				}

				Accept();
			});
		}

		// Reads one line, answers it, writes the answer, and goes round again.
		//
		// **Strictly one request in flight, by chaining rather than by a lock.**
		// The next read is started by the write's completion, so the responses
		// leave in the order the requests arrived - two writes outstanding on one
		// socket have no defined order between them.
		void Read() {
			asio::async_read_until(*Client, Incoming, '\n', [this](const asio::error_code &failed, size_t) {
				if (failed) {
					Drop();
					return;
				}

				std::istream stream(&Incoming);
				std::string line;
				std::getline(stream, line);

				// A bare carriage return survives `getline` on a client that
				// writes CRLF. Left on, it lands inside the JSON and every
				// parse fails for a reason nothing reports.
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}

				if (line.empty()) {
					Read();
					return;
				}

				std::string answer = Answering != nullptr ? (*Answering)(line) : std::string();
				Count++;

				if (answer.empty()) {
					// A notification. JSON-RPC forbids answering one, and MCP
					// sends `notifications/initialized` during every handshake.
					Read();
					return;
				}

				answer.push_back('\n');
				Outgoing = std::move(answer);

				asio::async_write(
					*Client, asio::buffer(Outgoing), [this](const asio::error_code &wrote, size_t) {
						if (wrote) {
							Drop();
							return;
						}
						Read();
					}
				);
			});
		}

		// Closes the client, if there is one, and forgets what it had said.
		void Drop() {
			if (Client != nullptr) {
				asio::error_code ignored;
				Client->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
				Client->close(ignored);
				Client.reset();
			}

			Incoming.consume(Incoming.size());
		}
	};

	Server::Server() : State(std::make_unique<Impl>()) {}

	Server::~Server() {
		Stop();
	}

	bool Server::Start(uint16_t port) {
		if (State->Running) {
			return true;
		}

		// **Loopback, spelled out.** `tcp::v4()` with any address is the usual
		// incantation and is exactly what must not happen here - see the
		// header.
		const asio::ip::tcp::endpoint local(asio::ip::make_address("127.0.0.1"), port);

		try {
			State->Acceptor = std::make_unique<asio::ip::tcp::acceptor>(State->Context);
			State->Acceptor->open(local.protocol());
			State->Acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
			State->Acceptor->bind(local);
			State->Acceptor->listen();
			State->Bound = State->Acceptor->local_endpoint().port();
		} catch (const std::exception &failure) {
			ENGINE_ERROR("control: could not listen on 127.0.0.1:{} - {}", port, failure.what());
			State->Acceptor.reset();
			return false;
		}

		// A context that has been stopped refuses to run anything until it is
		// told the run is a new one. Starting after a `Stop` is what needs this.
		State->Context.restart();

		State->Running = true;
		State->Accept();

		ENGINE_INFO("control: listening on 127.0.0.1:{}", State->Bound);
		return true;
	}

	void Server::Stop() {
		State->Running = false;

		// **Nothing is joined and nothing is woken, because nothing ever
		// blocked.** Closing the client and the acceptor completes their
		// outstanding operations with an error, and the final `poll` runs those
		// completions so no handler is left holding a socket. All of it on this
		// thread, none of it able to wait.
		State->Drop();

		if (State->Acceptor) {
			asio::error_code ignored;
			State->Acceptor->close(ignored);
		}

		State->Answering = nullptr;
		State->Context.poll();
		State->Context.stop();

		State->Acceptor.reset();
	}

	bool Server::IsRunning() const {
		return State->Running;
	}

	uint16_t Server::Port() const {
		return State->Bound;
	}

	bool Server::IsConnected() const {
		return State->Client != nullptr;
	}

	size_t Server::Served() const {
		return State->Count;
	}

	void Server::Pump(const Handler &handler) {
		if (!State->Running) {
			return;
		}

		// **`poll`, never `run`.** `run` would block this thread until the work
		// ran out, which for a listening socket is never. `poll` runs what is
		// ready and returns, so the editor gives the socket one slice a frame
		// and keeps drawing.
		State->Answering = &handler;
		State->Context.poll();
		State->Answering = nullptr;
	}
}
