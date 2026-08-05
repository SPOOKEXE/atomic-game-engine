#pragma once

// A JSON-RPC listener on loopback, so something outside this process can watch
// the editor and drive it.
//
// **The protocol is Model Context Protocol and the transport is a socket rather
// than stdio**, which is the one place this departs from how MCP servers are
// usually built. MCP's own transport is a subprocess speaking newline-delimited
// JSON-RPC on stdin and stdout; that shape cannot work here, because the thing
// being driven is a windowed program with a renderer and a universe, and it is
// started by a person or by `just edit` rather than by whatever wants to talk to
// it. So the editor listens and `mono.tools/mcpbridge` is the subprocess: it
// pumps bytes between stdio and this port and understands none of them.
//
// **Loopback only, and that is not configuration.** The acceptor binds
// `127.0.0.1` — an editor that answered on a routable address would be a
// remote-control surface for a program that runs scripts and writes files, on a
// developer's machine, with no authentication.
//
// **Nothing here knows what a request means.** This class owns a thread, a
// socket and a queue of strings; `control::Surface` turns one into an answer, on
// whichever thread called `Pump` — which is the program's main thread, because
// that is the only one allowed to look at a world. `Universe::Enter` aborts on re-entry rather than
// allowing it, so a socket thread reaching into a store would not race — it
// would abort the process. `Pump` is the seam: the socket thread parks a request
// and waits, the editor thread answers it between frames, and the answer goes
// back out.
//
// One client at a time. MCP is one client by construction, and a second
// connection is a second thing driving the same editor — accepted and then
// closed, rather than interleaved into the same queue.
//
// @tier shared

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine::control {

	// The listener, its thread, and the queue between them.
	//
	// @since v0.8
	class Server final {
	  public:
		// What turns one request into one response.
		//
		// Takes and returns a whole JSON-RPC message. Returning an empty string
		// means "no response", which is what a notification gets — JSON-RPC
		// forbids answering one, and MCP sends `notifications/initialized`
		// during every handshake.
		using Handler = std::function<std::string(const std::string &)>;

		Server();
		~Server();

		Server(const Server &) = delete;
		Server &operator=(const Server &) = delete;

		// Binds loopback and starts accepting.
		//
		// @param port The TCP port. Zero picks one, readable from `Port()`.
		// @return `false` when the port could not be bound, which is usually a
		//         second editor already listening on it.
		bool Start(uint16_t port);

		// Closes the socket and joins the thread. Safe to call twice, and safe
		// to call when `Start` failed.
		void Stop();

		// Whether the thread is up.
		bool IsRunning() const;

		// The port actually bound, which is what `Start(0)` is for.
		uint16_t Port() const;

		// Whether a client is connected right now.
		bool IsConnected() const;

		// Answers everything queued since the last call.
		//
		// **Called from the editor's frame loop and nowhere else.** Each
		// request is handed to `handler` on this thread, and the socket thread
		// is released once the answer is stored — so a slow tool blocks its own
		// caller and never the editor for longer than the call takes.
		//
		// @param handler What answers a request.
		void Pump(const Handler &handler);

		// How many requests have been answered since the server started.
		size_t Served() const;

	  private:
		// asio lives in the .cpp. Nothing that includes this header compiles a
		// networking library, which is the same trade `ui::Interface` makes for
		// imgui and `render` makes for SDL.
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
