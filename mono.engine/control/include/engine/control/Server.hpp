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
// **Nothing here knows what a request means.** This class owns a socket;
// `control::Surface` turns a line into an answer, on whichever thread called
// `Pump` — which is the program's main thread, because that is the only one
// allowed to look at a world. `Universe::Enter` aborts on re-entry rather than
// allowing it, so a socket thread reaching into a store would not race — it
// would abort the process.
//
// **There is no socket thread.** Every operation is asynchronous and nothing is
// ever blocked on: `Pump` polls the reactor from the editor's frame loop, so a
// completion runs on the editor's thread because that is the thread that ran it.
// The rule above therefore costs nothing to keep. It also means this can never
// be what hangs a shutdown — an operation that does not block cannot be stuck,
// so `Stop` has nothing to wake and no thread to join.
//
// One client at a time. MCP is one client by construction, and a second
// connection is a second thing driving the same editor — accepted and then
// closed immediately, rather than left established and unanswered.
//
// @tier shared

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine::control {

	// The listener, and the one client it serves.
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

		// Closes the client and the acceptor. Safe to call twice, and safe to
		// call when `Start` failed.
		//
		// **Cannot block.** There is no thread to join and nothing parked on a
		// socket, which is the whole reason the shutdown of a program embedding
		// this is not allowed to depend on whether a client happened to be
		// attached.
		void Stop();

		// Whether the socket is listening.
		bool IsRunning() const;

		// The port actually bound, which is what `Start(0)` is for.
		uint16_t Port() const;

		// Whether a client is connected right now.
		bool IsConnected() const;

		// Gives the socket one slice of this frame.
		//
		// **Called from the editor's frame loop and nowhere else, and it is the
		// only thing that makes the socket progress at all.** This polls the
		// reactor: whatever is ready — a connection, a line, a written response
		// — is run here and now, on this thread, and then it returns. Stop
		// calling it and the socket simply stops being read; nothing blocks and
		// nothing is lost, the client just waits.
		//
		// A request is handed to `handler` inside that poll, so a slow tool
		// costs the frame it runs in. That is the trade being made deliberately:
		// the alternative is answering a request off the editor's thread, which
		// is the one thing a world does not allow.
		//
		// @param handler What answers a request. Borrowed for the duration of
		//        the call and not retained.
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
