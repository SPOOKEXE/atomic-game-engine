#pragma once

// The origin, reachable. `cdn::Origin` decides what to serve; this is what a
// client can actually talk to.
//
// **This is the hop CDN.md §7 listed as missing** — "HTTP range serving, and the
// wire hop itself" — and it is why `mono.cdn` now links `Engine::net`. Before
// it, everything between a request and its compressed group was built and
// tested and nothing could reach any of it.
//
// The surface is six routes and no more:
//
// | Route | Answers |
// |---|---|
// | `GET /health` | that this process is up, and what it is serving |
// | `GET /manifest` | the signature and the manifest, in that order |
// | `GET /dictionary` | the trained dictionary, or 404 when there is none |
// | `GET /bundle/<root>` | one prepared group, against a grant |
// | `HEAD /ingest/<hash>` | whether that file is already here, against an ingest key |
// | `PUT /ingest/<hash>` | stores the body under that hash, against an ingest key |
//
// **The two write routes are off unless configured** — `IngestSettings` — and
// an origin with them off is byte-for-byte the read-only origin that existed
// before them. That is what makes "one server takes the writes, another serves
// the reads" a pair of config files rather than a pair of programs.
//
// **A path never becomes a filesystem path.** `/bundle/<root>` and
// `/ingest/<hash>` parse a 64-character hex hash or refuse; there is no route
// that takes a name, and there must not be. CDN.md §8: a request layer taking a
// path would have to repeat `ContentRoot`'s traversal checking, and a repeated
// check is one that will eventually differ. The ingest route is the place that
// rule earns its keep twice over, because it *writes*: the filename is built
// from the parsed hash and never from anything the client spelled.
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
#include <filesystem>
#include <memory>
#include <string>

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

		// Every byte written to a socket, and every byte read off one.
		//
		// **Counted apart from `ServedBytes`, which is a different measurement
		// and not a subset of a subset.** `ServedBytes` is the compressed group
		// payload a client asked for; these two are what the interface moved,
		// which also carries manifests, dictionaries, health checks, refusals
		// and every response's headers. An operator watching bandwidth wants
		// these; an operator asking what delivery cost wants the other, and one
		// number cannot answer both.
		//
		// `Engine::net` measures both at the socket, so a request that never
		// finished arriving is still counted — see `http::ServeReport`.
		uint64_t SentBytes = 0;

		// Bytes read off sockets.
		uint64_t ReceivedBytes = 0;

		// Files accepted at `/ingest/<hash>`.
		//
		// @since v0.10
		uint64_t Ingested = 0;

		// Uploads that named content already in the inbox.
		//
		// **Counted apart from an accepted one**, and it is the counter an
		// operator actually watches: a publisher that re-uploads its whole tree
		// every run should be almost entirely this, and a number near zero
		// means the `HEAD` probe that is supposed to skip them is not working.
		//
		// @since v0.10
		uint64_t IngestDuplicates = 0;

		// Bytes written to the inbox, uploads that were already there excluded.
		//
		// @since v0.10
		uint64_t IngestedBytes = 0;

		// Uploads refused: no key, a wrong key, a body that did not hash to the
		// target, or a service not accepting writes at all.
		//
		// **One counter, unlike the read side's `Refused`/`Missing` split.** On
		// the read path those separate a misconfigured client from somebody
		// probing; here every one of them is the same event — something tried
		// to write and was not allowed to — and the reason belongs in the log,
		// where it does not have to be answered back to the caller.
		//
		// @since v0.10
		uint64_t IngestRefused = 0;
	};

	// Whether this origin accepts uploads, and on what terms.
	//
	// **Off unless a key is set, and that default is the feature.** An origin
	// reachable on a network that accepts writes because somebody left a field
	// blank is an open dropbox, so "no key" means "no writes" rather than "no
	// check" — the same shape `DeliverySettings` uses for its publisher key,
	// where absent trust fetches nothing rather than trusting everything.
	//
	// **What the key does and does not buy.** It is admission and nothing more:
	// it says who may spend this origin's disk. It is *not* what makes an
	// upload trustworthy — the target is a content address and the service
	// hashes what arrives, so a client that uploads bytes not matching the hash
	// it named is refused whatever key it holds. That is why this can be a
	// shared secret rather than a signature: forging it gets you the ability to
	// store your own bytes under their own true names, not the ability to
	// substitute anybody else's.
	//
	// **Nothing published here is trusted by a reader either.** A client
	// verifies against a manifest the publisher signed — CDN.md §1 — so content
	// that reached this inbox still has to be published and signed before any
	// client will look at it. An ingest key that leaks costs disk, not trust.
	//
	// @since v0.10
	struct IngestSettings {
		// Where accepted files land, named `<hash>` with the extension the
		// uploader gave. Empty refuses every upload.
		//
		// This is `LocalPaths::Raw`'s shape and deliberately so: what arrives
		// here is what `PublishLocal` reads.
		std::filesystem::path Inbox;

		// The shared secret an uploader sends as `x-atomic-ingest`. Empty
		// refuses every upload, whatever `Inbox` says.
		std::string Key;

		// The largest single file this origin will take.
		//
		// **This bounds a socket buffer and not just a file.** A request is
		// parsed whole, so `Serve` raises `ConnectionBufferBytes` to fit one of
		// these — which means a large value here is a large value times however
		// many connections are uploading at once. Sixteen megabytes covers a
		// texture, a mesh or a script; an hour of audio needs it raised, and
		// raising it is a deliberate act rather than a default somebody
		// inherits.
		uint64_t MaximumFileBytes = 16ull * 1024u * 1024u;

		// Whether this describes an origin that accepts anything at all.
		bool Accepts() const;
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

		// Whether this origin takes uploads. Off by default.
		//
		// **A write origin and a read origin are the same program with
		// different settings**, which is what lets one machine take every write
		// and a second serve every read without either being a special build.
		// Separating them is then a deployment decision and a
		// `delivery::SourceRole` on the client, rather than two codebases.
		//
		// @since v0.10
		IngestSettings Ingest;
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
