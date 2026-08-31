#pragma once

// Asynchronous WebSocket server for userland tools and content services.
//
// The implementation owns its Asio context and invokes handlers from its
// worker thread. The public surface contains values and callbacks only, so the
// vendor transport remains private to net.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace engine::net::websocket {

	// Stable identity for one accepted peer.
	using ConnectionId = uint64_t;

	// Resource limits for a WebSocket listener.
	struct ServerSettings {
		// The maximum number of handshaken and handshaking peers together.
		size_t MaximumConnections = 10000;

		// Maximum payload in one data frame.
		size_t MaximumFrameBytes = 1024u * 1024u;

		// Number of Asio worker threads. One keeps callback order deterministic.
		size_t WorkerThreads = 1;
	};

	// Events delivered in connection order by the server worker.
	struct Callbacks {
		// Connection, frame, and disconnection handlers.
		//@{
		std::function<void(ConnectionId)> Open;
		std::function<void(ConnectionId, std::span<const std::byte>, bool)> Message;
		std::function<void(ConnectionId)> Close;
		//@}
	};

	// Owns a running asynchronous WebSocket listener.
	class Server {
	  public:
		virtual ~Server() = default;

		// Returns the bound local endpoint.
		virtual Endpoint Local() const = 0;
		// Reports whether the listener remains open.
		virtual bool Open() const = 0;
		// Returns the current peer count.
		virtual size_t Connections() const = 0;

		// Queues one server-to-client frame without blocking the caller.
		virtual bool
		Send(ConnectionId connection, std::span<const std::byte> payload, bool binary = false) = 0;

		// Closes a peer asynchronously. Repeated calls are harmless.
		virtual void Close(ConnectionId connection) = 0;
	};

	// Starts a listener on `port`, returning null when setup fails.
	std::unique_ptr<Server> Listen(uint16_t port, Callbacks callbacks, const ServerSettings &settings = {});
}
