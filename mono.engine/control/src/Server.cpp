// The control socket, and the thread that owns it.
//
// **Synchronous asio rather than asynchronous, and that is deliberate.** One
// client, one thread, one request in flight: the asynchronous form buys
// concurrency this has no use for and costs a completion handler around every
// read. What the thread does is block on a read, park the line, block on the
// editor, write the answer, and go round again.
//
// The framing is one JSON object per line, which is MCP's own stdio framing.
// `mono.tools/mcpbridge` copies bytes between stdio and this socket without
// parsing them, so the two ends agree by construction.

#include <engine/control/Server.hpp>
#include <engine/core/Log.hpp>

#include <asio.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace engine::control {

	namespace {
		// A request parked by the socket thread until the editor answers it.
		//
		// **The answer travels back in the same object rather than through a
		// second queue**, because the socket thread has to write the responses
		// in the order it read the requests, and two queues would need the pair
		// rebuilding at the far end.
		struct Pending {
			std::string Request;
			std::string Response;
			bool Answered = false;
		};
	}

	struct Server::Impl {
		asio::io_context Context;
		std::unique_ptr<asio::ip::tcp::acceptor> Acceptor;
		std::thread Thread;

		std::mutex Lock;
		std::condition_variable Answered;
		std::deque<std::shared_ptr<Pending>> Queue;

		std::atomic<bool> Running{false};
		std::atomic<bool> Connected{false};
		std::atomic<size_t> Count{0};
		uint16_t Bound = 0;

		// Parks one line and waits for the editor to answer it.
		//
		// **Returns empty when the server is stopping**, which is what makes
		// Stop able to join: a socket thread blocked forever on a request the
		// editor will never pump would outlive the editor that was supposed to
		// answer it.
		std::string Ask(std::string line) {
			auto pending = std::make_shared<Pending>();
			pending->Request = std::move(line);

			{
				std::lock_guard<std::mutex> guard(Lock);
				Queue.push_back(pending);
			}

			std::unique_lock<std::mutex> guard(Lock);
			Answered.wait(guard, [&] { return pending->Answered || !Running.load(); });
			return pending->Answered ? std::move(pending->Response) : std::string();
		}

		void Serve(asio::ip::tcp::socket socket) {
			Connected.store(true);
			asio::streambuf buffer;

			while (Running.load()) {
				asio::error_code failed;
				const size_t read = asio::read_until(socket, buffer, '\n', failed);
				if (failed || read == 0) {
					break;
				}

				std::istream stream(&buffer);
				std::string line;
				std::getline(stream, line);

				// A bare carriage return survives `getline` on a client that
				// writes CRLF. Left on, it lands inside the JSON and every
				// parse fails for a reason nothing reports.
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.empty()) {
					continue;
				}

				std::string answer = Ask(std::move(line));
				if (answer.empty()) {
					// A notification, or a stopping server. Neither is written.
					continue;
				}

				answer.push_back('\n');
				asio::write(socket, asio::buffer(answer), failed);
				if (failed) {
					break;
				}
			}

			Connected.store(false);
		}

		void Run() {
			while (Running.load()) {
				asio::error_code failed;
				asio::ip::tcp::socket socket(Context);
				Acceptor->accept(socket, failed);

				if (failed) {
					// The acceptor was closed by Stop, or the listen backlog
					// failed. Either way there is nothing to serve.
					break;
				}

				// **Nagle off.** Every message here is one small line and the
				// far end is waiting for it; a 40ms coalescing delay on a
				// request/response protocol is the whole latency budget.
				socket.set_option(asio::ip::tcp::no_delay(true), failed);

				Serve(std::move(socket));
			}
		}
	};

	Server::Server() : State(std::make_unique<Impl>()) {}

	Server::~Server() {
		Stop();
	}

	bool Server::Start(uint16_t port) {
		if (State->Running.load()) {
			return true;
		}

		// **Loopback, spelled out.** `tcp::v4()` with any address is the usual
		// incantation and is exactly what must not happen here — see the
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
			ENGINE_ERROR("control: could not listen on 127.0.0.1:{} — {}", port, failure.what());
			State->Acceptor.reset();
			return false;
		}

		State->Running.store(true);
		State->Thread = std::thread([this] { State->Run(); });

		ENGINE_INFO("control: listening on 127.0.0.1:{}", State->Bound);
		return true;
	}

	void Server::Stop() {
		if (!State->Running.exchange(false)) {
			// Never started, or already stopped. The thread may still be
			// joinable from a failed start, so fall through to the join.
			if (!State->Thread.joinable()) {
				return;
			}
		}

		if (State->Acceptor) {
			asio::error_code ignored;
			State->Acceptor->close(ignored);
		}

		// **Wake anything parked before joining.** A socket thread inside `Ask`
		// is waiting on a condition variable the editor will not signal again,
		// and joining without this hangs the shutdown of the whole program.
		State->Answered.notify_all();

		if (State->Thread.joinable()) {
			State->Thread.join();
		}

		State->Acceptor.reset();
		State->Connected.store(false);
	}

	bool Server::IsRunning() const {
		return State->Running.load();
	}

	uint16_t Server::Port() const {
		return State->Bound;
	}

	bool Server::IsConnected() const {
		return State->Connected.load();
	}

	size_t Server::Served() const {
		return State->Count.load();
	}

	void Server::Pump(const Handler &handler) {
		for (;;) {
			std::shared_ptr<Pending> pending;
			{
				std::lock_guard<std::mutex> guard(State->Lock);
				if (State->Queue.empty()) {
					return;
				}
				pending = State->Queue.front();
				State->Queue.pop_front();
			}

			// **Outside the lock**, because a tool can take a while — a
			// screenshot encodes an image — and holding the queue lock across
			// it would stall the socket thread's next read for no reason.
			std::string answer = handler(pending->Request);

			{
				std::lock_guard<std::mutex> guard(State->Lock);
				pending->Response = std::move(answer);
				pending->Answered = true;
			}

			State->Count.fetch_add(1);
			State->Answered.notify_all();
		}
	}
}
