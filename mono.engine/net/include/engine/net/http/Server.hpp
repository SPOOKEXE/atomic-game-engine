#pragma once

// The listening half: a TCP port that turns bytes into `http::Request` values
// and writes back what a handler returned.
//
// **This exists because a group is megabytes and a datagram is 1200 bytes.**
// `Transport` carries whole datagrams for a simulation with a per-tick packet
// budget; a content origin ships bulk bytes and wants ordering, retransmission
// and flow control from the operating system rather than from `Reliability`.
// CDN.md §5 and `ROADMAP.md` v0.9 both name this as the missing hop.
//
// **Polled, never threaded, and completions land on the caller's thread.**
// `Pump` drives everything: accepting, reading, dispatching and writing all
// happen inside it and on the thread that called it. That is the same contract
// `cdn::Origin::Pump` already has, and it is what lets an origin, a server
// attachment and a test all drive one of these without any of them agreeing
// about threads first.
//
// This is allowed to be asynchronous where `Transport` is not, and the reason is
// written in CDN.md §3: **the origin has no tick.** `AGENTS.md` rule 5 governs
// work inside a tick, and a request that completes a poll later changes nothing
// a recorded run would have to reproduce. A datagram arriving on somebody else's
// thread mid-tick is a desync; a byte of a group arriving between two polls is
// not.
//
// **Nothing here blocks, including `Close`.** A handler that wants to read a
// file should already have the bytes: this calls it while a connection is open
// and every other connection is waiting.
//
// **The bounds are the security surface.** A public port with no connection
// ceiling, no per-connection buffer bound and no message size limit is a
// denial-of-service primitive with a friendly name, so all three are in
// `ServerSettings` and none of them is optional.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Message.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace engine::net::http {

	// How a server is sized and bounded.
	//
	// @since v0.9
	struct ServerSettings {
		// The bounds every message parse is held to.
		MessageLimits Limits;

		// How many connections may be open at once.
		//
		// A new connection past this is accepted and closed immediately rather
		// than left un-accepted: leaving it in the kernel's backlog makes a
		// client wait for a timeout to learn something this end already knows.
		size_t MaximumConnections = 64;

		// How many bytes one connection may have buffered inbound before it is
		// dropped.
		//
		// Separate from `MessageLimits` because that bounds one message and
		// this bounds a peer that never finishes one.
		size_t ConnectionBufferBytes = 64u * 1024u;

		// How many completed requests one `Pump` will dispatch.
		//
		// Bounded for `cdn::Origin::PreparePerPump`'s reason: a burst must not
		// make one pump run for an unbounded time and starve whatever else the
		// calling thread does.
		size_t DispatchPerPump = 32;

		// How many polls a connection may go without progress before it is
		// dropped.
		//
		// Counted in polls rather than in wall time, so a suite states a
		// timeout instead of sleeping for one — `net/AGENTS.md`'s standing rule
		// that time is passed in rather than read. Zero disables it.
		uint32_t IdlePolls = 0;
	};

	// What a `Pump` did.
	//
	// @since v0.9
	struct ServeReport {
		// Connections accepted this poll.
		size_t Accepted = 0;

		// Requests handed to the handler and answered.
		size_t Served = 0;

		// Connections closed because their bytes were not a message this
		// subset accepts.
		//
		// Counted apart from an ordinary close for `assets::Grant`'s reason:
		// a peer speaking something else is an ordinary event and a peer
		// speaking almost-HTTP is somebody trying something, and one counter
		// for both buries the second in the first.
		size_t Rejected = 0;

		// Connections closed for any ordinary reason — the peer left, the
		// response is written and `connection: close` was asked for, or the
		// idle bound was reached.
		size_t Closed = 0;
	};

	// A listening HTTP port.
	//
	// **One owner, one thread**, the same as `Transport` and for the same
	// reason: a socket does not survive two threads calling into it at once,
	// and a class that pretended otherwise would pass a test the real thing
	// fails.
	//
	// @since v0.9
	class Server {
	  public:
		// What answers a request.
		//
		// Called inside `Pump`, on the pumping thread, with every other
		// connection waiting — so it must not block. Returning a `Response`
		// with an unset `Code` is answered `500`, because a handler that fell
		// through is a bug here rather than a message to send.
		using Handler = std::function<Response(const Request &)>;

		virtual ~Server() = default;

		// Accepts, reads, dispatches and writes, without blocking.
		//
		// @param handler What answers a request.
		// @return What happened this poll.
		virtual ServeReport Pump(const Handler &handler) = 0;

		// The address this server accepts on.
		//
		// Worth asking for even when the port was chosen: a port of zero binds
		// an ephemeral one, and this is the only way to learn which — which is
		// exactly what a test needs so it does not have to pick a number and
		// hope nothing else on the machine wanted it.
		//
		// @return The local endpoint, or an invalid one once closed.
		virtual Endpoint Local() const = 0;

		// How many connections are open.
		virtual size_t Connections() const = 0;

		// Whether this is still listening.
		virtual bool Open() const = 0;

		// Stops listening and drops every open connection.
		virtual void Close() = 0;
	};

	// Binds a listening socket on every IPv4 interface.
	//
	// @param port The port to bind, or zero for an ephemeral one. `Local` says
	//        which was chosen.
	// @param settings How to size and bound it.
	// @return The server, or nothing when the socket could not be created or
	//         the port could not be bound. A null rather than an exception,
	//         because a port already in use is an ordinary outcome of starting
	//         a second origin on one machine.
	// @since v0.9
	std::unique_ptr<Server> Listen(uint16_t port, const ServerSettings &settings = {});
}
