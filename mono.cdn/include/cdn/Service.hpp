#pragma once

// The origin, reachable. `cdn::Origin` decides what to serve; this is what a
// client can actually talk to.
//
// **This is the hop CDN.md §7 listed as missing** — "HTTP range serving, and the
// wire hop itself" — and it is why `mono.cdn` now links `Engine::net`. Before
// it, everything between a request and its compressed group was built and
// tested and nothing could reach any of it.
//
// The surface is four routes and no more:
//
// | Route | Answers |
// |---|---|
// | `GET /health` | that this process is up, and what it is serving |
// | `GET /manifest` | the signature and the manifest, in that order |
// | `GET /dictionary` | the trained dictionary, or 404 when there is none |
// | `GET /bundle/<root>` | one prepared group, against a grant |
//
// **A path never becomes a filesystem path.** `/bundle/<root>` parses a
// 64-character hex hash or refuses; there is no route that takes a name, and
// there must not be. CDN.md §8: a request layer taking a path would have to
// repeat `ContentRoot`'s traversal checking, and a repeated check is one that
// will eventually differ.
//
// **A refusal says nothing about which check failed.** `cdn::Gate`'s rule
// reaching the wire: the counters distinguish a forged grant from an expired
// one for an operator, and the client gets `403` either way, because a reason
// returned to a client is an oracle.
//
// **Serving is same-thread and synchronous inside a pump.** Preparing a group
// is CPU work with a known end — hashing and compressing a known set — which is
// exactly what `Origin::Pump` is built for and exactly what `Jobs::For` is
// allowed to fan out. It is not IO: the payload is resolved before the fan-out,
// which is the arrangement `mono.cdn/AGENTS.md` requires.
//
// @tier shared

#include <engine/assets/ChunkStore.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Server.hpp>

#include <cdn/Origin.hpp>
#include <cstdint>
#include <memory>

namespace cdn {

	// What a service has answered.
	//
	// @since v0.9
	struct ServiceCounters {
		// Health checks.
		uint64_t Health = 0;

		// Manifests served.
		uint64_t Manifests = 0;

		// Dictionaries served.
		uint64_t Dictionaries = 0;

		// Groups served.
		uint64_t Bundles = 0;

		// Compressed bytes handed to clients.
		uint64_t ServedBytes = 0;

		// Requests the gate refused.
		//
		// **Counted apart from a miss.** A client asking for content it was not
		// granted and a client asking for content that is not here are
		// different events, and one counter for both means an operator cannot
		// tell a misconfigured deployment from somebody probing it.
		uint64_t Refused = 0;

		// Requests for content this origin does not have.
		uint64_t Missing = 0;

		// Requests for a route that does not exist, or a malformed one.
		uint64_t Rejected = 0;
	};

	// How a service is set up.
	//
	// @since v0.9
	struct ServiceSettings {
		// The port to listen on. Zero binds an ephemeral one, which is what a
		// test wants so it does not have to pick a number and hope.
		uint16_t Port = 0;

		// How the listening socket is sized and bounded.
		engine::net::http::ServerSettings Server;
	};

	// An origin on a port.
	//
	// **One owner, one thread**, the same as everything else that owns a
	// socket.
	//
	// @since v0.9
	class Service {
	  public:
		virtual ~Service() = default;

		// Accepts, answers and prepares, without blocking.
		//
		// @param nowSeconds The current time, on the clock shared with the
		//        server that issues grants. Passed in rather than read, so this
		//        holds no notion of "now" of its own to drift — `assets::Grant`
		//        and `net`'s standing rule.
		// @return How many requests were answered.
		virtual size_t Pump(uint64_t nowSeconds) = 0;

		// The address this service accepts on.
		//
		// Worth asking for even when the port was chosen: zero binds an
		// ephemeral one and this is the only way to learn which.
		virtual engine::net::Endpoint Local() const = 0;

		// What it has answered.
		virtual const ServiceCounters &Counters() const = 0;

		// Whether it is still listening.
		virtual bool Open() const = 0;

		// Stops listening.
		virtual void Close() = 0;
	};

	// Starts serving an origin on a port.
	//
	// @param origin What decides admission and prepares groups. Borrowed: it
	//        must outlive the service, because a publication swap is the
	//        origin's to make and a service holding a copy would serve content
	//        that had been replaced.
	// @param store Where payloads and the published manifest are read from.
	// @param settings The port and the socket's bounds.
	// @return The service, or nothing when the port could not be bound — an
	//         ordinary outcome of starting a second origin on one machine.
	// @since v0.9
	std::unique_ptr<Service>
	Serve(Origin &origin, engine::assets::ChunkStore store, const ServiceSettings &settings = {});
}
