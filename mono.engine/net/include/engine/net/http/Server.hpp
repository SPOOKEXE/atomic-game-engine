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
		// What a single request may be, in size and in shape.
		//
		// Held apart from the connection limits below because the two bound
		// different attacks: one peer sending something enormous, and many peers
		// sending nothing at all.
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
		// Connections taken this pump.
		size_t Accepted = 0;

		// Requests answered.
		//
		// **Below `Accepted` is the ordinary case rather than a fault**: a
		// connection that has sent only part of a request is accepted and not
		// yet served, and will be on a later pump.
		size_t Served = 0;

		// Connections closed for malformed or unsupported requests.
		size_t Rejected = 0;

		// Connections that ended normally — the peer went away, or the idle
		// timeout took one.
		//
		// **Apart from `Rejected`, because the two mean opposite things about
		// the peer.** A rising `Rejected` is somebody sending what this server
		// will not parse; a rising `Closed` is ordinary traffic finishing.
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

		// How many connections are open right now.
		//
		// @return The count, which a caller weighs against
		//         `ServerSettings::MaximumConnections`.
		virtual size_t Connections() const = 0;

		// Whether the listening socket is still bound.
		//
		// @return `false` once `Close` has run, or if the bind failed.
		virtual bool Open() const = 0;

		// Stops listening and drops every open connection.
		//
		// **Idempotent, because the destructor calls it too** — an origin shut
		// down explicitly and then destroyed is the ordinary path, not a
		// mistake.
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
