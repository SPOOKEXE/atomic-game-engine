#pragma once

// Polled TCP listener for bounded HTTP request/response traffic.
//
// `Pump` performs all socket work and invokes handlers on its caller's thread.
// Connection, buffer and message limits bound denial-of-service exposure.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Message.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace engine::net::http {

	// Connection and request limits.
	//
	// @since v0.9
	struct ServerSettings {
		MessageLimits Limits;

		// Excess connections are accepted and closed immediately.
		size_t MaximumConnections = 64;

		// Bounds an incomplete request independently of message size.
		size_t ConnectionBufferBytes = 64u * 1024u;

		// Prevents one burst from monopolising the caller's thread.
		size_t DispatchPerPump = 32;

		// Poll-based timeout; zero disables it.
		uint32_t IdlePolls = 0;
	};

	// @since v0.9
	struct ServeReport {
		size_t Accepted = 0;

		size_t Served = 0;

		// Connections closed for malformed or unsupported requests.
		size_t Rejected = 0;

		size_t Closed = 0;

		// Payload bytes returned by socket reads, including incomplete requests.
		uint64_t ReceivedBytes = 0;

		// Payload bytes actually returned by socket writes.
		uint64_t SentBytes = 0;
	};

	// A single-owner, caller-thread-polled HTTP listener.
	//
	// @since v0.9
	class Server {
	  public:
		// Called by `Pump`; it must not block. An unset response code becomes 500.
		using Handler = std::function<Response(const Request &)>;

		virtual ~Server() = default;

		// Processes ready socket work without blocking.
		//
		// @param handler What answers a request.
		// @return What happened this poll.
		virtual ServeReport Pump(const Handler &handler) = 0;

		// Reports the port chosen when binding with zero.
		//
		// @return The local endpoint, or an invalid one once closed.
		virtual Endpoint Local() const = 0;

		virtual size_t Connections() const = 0;

		virtual bool Open() const = 0;

		virtual void Close() = 0;
	};

	// Binds an IPv4 listening socket.
	//
	// @param port The port to bind, or zero for an ephemeral one. `Local` says
	//        which was chosen.
	// @param settings How to size and bound it.
	// @return The server, or null when creation or binding fails.
	// @since v0.9
	std::unique_ptr<Server> Listen(uint16_t port, const ServerSettings &settings = {});
}
