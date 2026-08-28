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

	using ConnectionId = uint64_t;

	struct ServerSettings {
		// The maximum number of handshaken and handshaking peers together.
		size_t MaximumConnections = 10000;

		// Maximum payload in one data frame.
		size_t MaximumFrameBytes = 1024u * 1024u;

		// Number of Asio worker threads. One keeps callback order deterministic.
		size_t WorkerThreads = 1;
	};

	struct Callbacks {
		std::function<void(ConnectionId)> Open;
		std::function<void(ConnectionId, std::span<const std::byte>, bool)> Message;
		std::function<void(ConnectionId)> Close;
	};

	class Server {
	  public:
		virtual ~Server() = default;

		virtual Endpoint Local() const = 0;
		virtual bool Open() const = 0;
		virtual size_t Connections() const = 0;

		// Queues one server-to-client frame without blocking the caller.
		virtual bool
		Send(ConnectionId connection, std::span<const std::byte> payload, bool binary = false) = 0;

		// Closes a peer asynchronously. Repeated calls are harmless.
		virtual void Close(ConnectionId connection) = 0;
	};

	std::unique_ptr<Server> Listen(uint16_t port, Callbacks callbacks, const ServerSettings &settings = {});
}
