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

	// The port a program opens when it is told to listen and given no number.
	//
	// **Here because there were three of these and they disagreed.** The
	// editor's `--mcp-port` help said 8738, the panel's field offered 8720 and
	// `mcpbridge` dialled 8730, so somebody who followed the help got a bridge
	// talking to a closed port and an editor talking to nobody. `.mcp.json` and
	// `RUNNING.md` both say 8738, so 8738 is the one that was right.
	//
	// @since v0.19
	inline constexpr uint16_t DEFAULT_PORT = 8738;

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
