#pragma once

// A loopback JSON-RPC listener for editor control.
//
// `Pump` runs socket work on the caller's thread. The listener has no socket
// thread, accepts one client, and binds only to `127.0.0.1`.
//
// @tier shared

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine::control {

	// The editor's conventional control port, and what `mcpbridge` dials with
	// no `--port`.
	//
	// **Here because there were copies of this number and they disagreed.** The
	// editor's `--mcp-port` help said 8738, its saved preferences seeded the
	// panel's field with 8720 and `mcpbridge`'s own usage example still showed
	// 8730 at v0.19, so somebody who followed one of them got a bridge talking
	// to a closed port. `.mcp.json` and `RUNNING.md` both say 8738, so 8738 is
	// the one that was right.
	//
	// **The number outside C++ is checked rather than trusted.**
	// `mono.tools/mcpbridge/CMakeLists.txt` reads this declaration at configure
	// time and fails the build when `.mcp.json` or the `just mcp` recipe
	// disagrees with it - the three places a client is actually pointed at a
	// port. A fourth copy is a configure error rather than a support question.
	//
	// @since v0.19
	inline constexpr uint16_t DEFAULT_PORT = 8738;

	// A dedicated server's conventional control port.
	//
	// **A different number from the editor's, and that is the whole reason it
	// exists**: the two supported programs run on one machine while somebody is
	// building a game, and a shared default would mean whichever started second
	// failed to bind. Neither is enforced - any free port works - and neither is
	// opened unless `--mcp-port` asks.
	//
	// @since v0.19
	inline constexpr uint16_t DEFAULT_SERVER_PORT = 8734;

	// A listener with one client.
	//
	// @since v0.8
	class Server final {
	  public:
		// Takes and returns one JSON-RPC message. An empty result suppresses the response.
		using Handler = std::function<std::string(const std::string &)>;

		Server();
		~Server();

		Server(const Server &) = delete;
		Server &operator=(const Server &) = delete;

		// Binds loopback and starts accepting clients.
		//
		// @param port The TCP port. Zero picks one, readable from `Port()`.
		// @return `false` when the port could not be bound, which is usually a
		//         second editor already listening on it.
		bool Start(uint16_t port);

		// Closes the client and acceptor. Safe after a failed start and on repeat calls.
		void Stop();

		// Whether the socket is listening.
		bool IsRunning() const;

		// The port actually bound, which is what `Start(0)` is for.
		uint16_t Port() const;

		// Whether a client is connected right now.
		bool IsConnected() const;

		// Polls the socket and invokes `handler` on the caller's thread.
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
