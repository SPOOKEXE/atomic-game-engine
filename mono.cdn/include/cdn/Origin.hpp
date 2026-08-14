#pragma once

// The origin's request pipeline: nothing blocks, and nothing is decided here
// about who asked.
//
// **The origin has no tick, and that is why async is allowed.** AGENTS.md rule 5
// governs work inside a tick; this is not a simulation, has no store and no
// replay obligation, so a request that completes later changes nothing a
// recorded run would reproduce. CDN.md §3.
//
// The client side is the opposite and the boundary matters: a fetch issued from
// a world *does* live inside a tick, and there rule 5 applies with no exception
// - the completion is applied at the barrier. A chunk that becomes visible to a
// system mid-tick is a desync. That half belongs to `Engine::assets` and the
// world driver, not here.
//
// **Concurrency is over requests, not over cores.** `Jobs::For` blocks until
// done: right for a tick, wrong for a service whose work is waiting on a
// filesystem, because a construct that occupies a worker while it waits turns an
// IO-bound origin into a thread-starved one. The one place a fan-out job *is*
// right is preparing a group - hashing, chunking and compressing a known set is
// CPU work with a known end - and that is where this uses one.
//
// **A publication is immutable and publishing is an atomic swap.** A request
// that started against one set of content keeps it, whatever is published
// underneath. Mutating a live publication would hand a client a manifest naming
// chunks that are not there yet, and the failure arrives as a hash mismatch far
// from its cause.
//
// **Local content is looked at before anything external.** That one default is
// what makes an origin a *cache server* rather than a proxy, and CDN.md §6's
// three sources are three `CDNSettings` field combinations rather than three
// programs. What an upstream returns is checked against the local manifest
// before it is cached or served, because a proxy that forwards bytes it cannot
// check is a proxy that launders a compromised upstream.
//
// @tier shared

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/delivery/GroupCodec.hpp>

#include <cdn/ContentRoot.hpp>
#include <cdn/Gate.hpp>
#include <cdn/PreparedCache.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cdn {

	// One published set of content, fixed for its whole life.
	//
	// Built, then never changed. Everything that serves it holds a shared
	// pointer, so a publication stays alive for exactly as long as something is
	// still using it and no longer.
	class Publication {
	  public:
		// @param root The directory this content is served from.
		// @param manifest What it contains.
		// @param dictionary The dictionary its groups are compressed against, or
		//        nothing for uncompressed-dictionary groups.
		Publication(
			ContentRoot root,
			engine::assets::Manifest manifest,
			std::optional<engine::delivery::Dictionary> dictionary = std::nullopt
		);

		// The directory being served.
		const ContentRoot &Root() const {
			return Directory;
		}

		// What this publication contains.
		const engine::assets::Manifest &Contents() const {
			return Described;
		}

		// The dictionary groups are compressed against, or nullptr.
		const engine::delivery::Dictionary *CompressionDictionary() const {
			return Codebook ? &*Codebook : nullptr;
		}

		// The dictionary's hash, or all-zero when there is none.
		//
		// Half of a prepared group's cache key, so it is asked for often enough
		// to be worth answering without a branch at every call site.
		const engine::assets::ContentHash &DictionaryHash() const {
			return CodebookHash;
		}

	  private:
		ContentRoot Directory;
		engine::assets::Manifest Described;
		std::optional<engine::delivery::Dictionary> Codebook;
		engine::assets::ContentHash CodebookHash;
	};

	// A request in flight.
	//
	// A number rather than a pointer, for AGENTS.md rule 3's reason: this is
	// exactly the sort of handle that ends up crossing a boundary.
	struct RequestId {
		// The value meaning "no request". Zero is never issued.
		static constexpr uint64_t NONE = 0;

		// The origin's monotonic request counter.
		uint64_t Value = NONE;

		// Whether this names a request at all.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// Whether two handles name the same request.
		constexpr bool operator==(const RequestId &other) const = default;
	};

	// Where a request has got to.
	enum class RequestState : uint8_t {
		// Not a request this origin issued, or one already taken.
		Unknown,

		// Accepted and waiting for `Pump`.
		Pending,

		// Prepared. The frame is waiting to be taken.
		Ready,

		// Abandoned by the caller.
		Cancelled,

		// The gate refused it, or the content is not in the publication it was
		// admitted against.
		Refused,
	};

	// Returns a stable, human-readable name for a request state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(RequestState state);

	// Where a bundle's uncompressed bytes come from.
	//
	// **A seam, not a placeholder.** The on-disk chunk layout is a separate
	// thing this module deliberately does not decide - CDN.md §7 lists it as not
	// started - and wiring the pipeline directly to a filesystem would bake that
	// undecided layout into the request path. A source returns nothing when the
	// bundle's bytes are unavailable, which the pipeline treats as a refusal
	// rather than an empty group.
	using PayloadSource =
		std::function<std::optional<std::vector<std::byte>>(const engine::assets::ContentHash &)>;

	// One origin this one may forward a miss to.
	//
	// Named as well as addressed, because a name is what crosses into a
	// configuration file, a log line and a dashboard - AGENTS.md rule 4 - and
	// "upstream 2 timed out" is not something anybody can act on.
	struct UpstreamOrigin {
		// What it is called.
		std::string Name;

		// Where it is. Opaque here on purpose: `Engine::net` has no transport
		// yet, and the fetch seam takes this whole struct so an endpoint's shape
		// stays the transport's business rather than this module's.
		std::string Endpoint;
	};

	// How this origin behaves - the whole of its setup.
	//
	// **Serving locally before asking anyone else is what makes this a cache
	// server rather than a proxy**, and it is the default. A local hit costs a
	// lookup and no network at all, which is the entire reason to deploy one of
	// these next to a population of players.
	//
	// The three deployments CDN.md §6 names fall out of these fields rather than
	// out of three programs:
	//
	// | Deployment | Settings |
	// |---|---|
	// | Local store - the server serving its own disk | `AllowUpstream` off |
	// | Cache server - local first, upstream on a miss | `LocalFirst` on, `AllowUpstream` on |
	// | Pure proxy - always ask upstream | `LocalFirst` off, `AllowUpstream` on |
	struct CDNSettings {
		// Look in the local publication before asking any upstream.
		//
		// Off makes this a pure proxy that always forwards. That is a real
		// deployment - an edge node holding no content of its own - but it is
		// not the common one, and it is not the default.
		bool LocalFirst = true;

		// Whether a miss may be forwarded at all.
		//
		// **Off by default.** An origin that will fetch from elsewhere is an
		// origin that can be pointed at elsewhere, so turning it on is a
		// deliberate act rather than something a default arranges.
		bool AllowUpstream = false;

		// Keep what an upstream returned, so the second request is local.
		//
		// This is the whole of "cache server": without it, an origin forwards
		// the same bundle to the same upstream for every client that asks.
		bool CacheUpstream = true;

		// The upstreams, tried in this order.
		std::vector<UpstreamOrigin> Upstreams;

		// How many upstreams to try before refusing.
		//
		// Bounded rather than "all of them": a request that walks ten dead
		// upstreams has spent ten timeouts before it refuses, and the client
		// gave up long before.
		uint32_t UpstreamAttempts = 2;

		// Whether what an upstream returned must match the local manifest.
		//
		// **Leave this on.** A proxy that forwards bytes it cannot check is a
		// proxy that launders a compromised upstream - and the local manifest is
		// signed, so the check costs nothing but a comparison. See
		// `Origin::Pump` for exactly how much is checked today and what is still
		// missing.
		bool VerifyUpstream = true;

		// The most groups one `Pump` will prepare.
		//
		// A bound rather than "everything outstanding", so that a burst of
		// requests cannot make one pump run for an unbounded time and starve
		// whatever else the calling thread does.
		size_t PreparePerPump = 8;

		// The compression level groups are prepared at.
		int CompressionLevel = engine::delivery::GroupCodec::DEFAULT_LEVEL;

		// What the prepared-group cache may hold.
		uint64_t CacheCapacityBytes = PreparedCache::DEFAULT_CAPACITY_BYTES;

		// Whether these can be used.
		//
		// Forwarding with no upstreams configured is refused: it reads as "this
		// will forward" and behaves as "this refuses every miss", and the gap
		// between those two is a deployment that looks healthy and serves
		// nothing.
		bool IsValid() const;
	};

	// How this origin reaches an upstream.
	//
	// A seam for the same reason `PayloadSource` is one: `Engine::net` has no
	// transport yet, and wiring the request path to a socket that does not exist
	// would bake an undecided shape into it.
	//
	// Returns the bundle's **uncompressed** bytes, not a prepared frame. An
	// upstream's dictionary is not necessarily this origin's, and caching a frame
	// compressed against someone else's dictionary under this origin's key would
	// hand a client bytes it cannot decode.
	using UpstreamFetch = std::function<
		std::optional<std::vector<std::byte>>(const UpstreamOrigin &, const engine::assets::ContentHash &)>;

	// Accepts requests, prepares groups, and hands back frames.
	//
	// Not thread-safe as a whole: `Pump` and the request calls are meant to run
	// on the origin's own thread. The `PreparedCache` underneath *is*
	// thread-safe, because it is the piece several origins or several pumps
	// would share.
	class Origin {
	  public:
		// @param key The secret shared with the server that issues grants.
		// @param settings The whole of how this origin behaves.
		explicit Origin(engine::assets::GrantKey key, const CDNSettings &settings = {});

		// Replaces the current publication, atomically.
		//
		// Requests already accepted keep the publication they were admitted
		// against - that is the whole point of the swap. The prepared cache is
		// cleared, because the previous publication's groups were compressed
		// against content and a dictionary that are no longer current.
		//
		// @param publication What to serve now. Null is refused; an origin with
		//        nothing to serve refuses requests rather than serving nothing.
		// @return Whether it was published.
		bool Publish(std::shared_ptr<const Publication> publication);

		// What is being served now.
		//
		// @return The publication, or nullptr before the first Publish.
		std::shared_ptr<const Publication> Current() const;

		// Admits a request, or refuses it.
		//
		// The gate decides - the MAC, the expiry, then the scope - and this
		// learns nothing about who asked. A refused request still gets a handle
		// so a caller can ask why once, rather than a bare failure that carries
		// no state.
		//
		// @param token The grant the client presented.
		// @param bundleRoot The bundle being asked for.
		// @param nowSeconds The current time, on the clock shared with the server.
		// @return A handle. Always valid; ask `StateOf` what happened.
		RequestId Submit(
			std::span<const std::byte> token,
			const engine::assets::ContentHash &bundleRoot,
			uint64_t nowSeconds
		);

		// Where a request has got to.
		//
		// @param id The request.
		// @return Its state, or `Unknown` for a handle this origin did not issue
		//         or has already handed the result of.
		RequestState StateOf(RequestId id) const;

		// Abandons a request.
		//
		// **Cancellation is load-bearing, not a convenience.** The absence of it
		// is what produces a game that hitches every time a player turns around
		// - DATATYPES_LIBRARIES.md on the `assets` surface. A cancelled request
		// is never prepared, and one cancelled mid-preparation has its result
		// discarded rather than delivered to nobody.
		//
		// @param id The request to abandon.
		// @return False for an unknown request or one already finished.
		bool Cancel(RequestId id);

		// Prepares outstanding requests, up to `PreparePerPump`.
		//
		// Four stages, and the split between them is the design rather than
		// tidiness:
		//
		// 1. **Cache lookup**, on one thread. A hit costs a lookup and no
		//    compression at all.
		// 2. **Resolve the payload** - local first, then upstream, per
		//    `CDNSettings`. This is the IO, and it is deliberately *outside* the
		//    fan-out below: a construct that occupies a worker while it waits on
		//    a filesystem or a socket turns an IO-bound origin into a
		//    thread-starved one. CDN.md §3.
		// 3. **Compress**, with `Jobs::For`. The one place a fan-out job is right
		//    here, because compressing a known set of groups is CPU work with a
		//    known end.
		// 4. **Publish**, on one thread, because the cache and the request table
		//    are shared and this is the cheap half.
		//
		// **What an upstream returns is checked against the local manifest**
		// when `VerifyUpstream` is on: its length must match what the signed
		// manifest records for that bundle. That is a real check against signed
		// data and it is *not* the whole of one - chunk-level verification needs
		// the chunk layout inside a group, which is not designed yet. A client
		// verifies end to end regardless, so this is defence in depth rather than
		// the trust boundary; the boundary is still the client's.
		//
		// @param source Where a bundle's local bytes come from.
		// @param upstream How to reach an upstream, or empty for none. Ignored
		//        unless `CDNSettings::AllowUpstream` is on.
		// @return How many requests reached `Ready` or `Refused`.
		size_t Pump(const PayloadSource &source, const UpstreamFetch &upstream = {});

		// Takes a ready request's frame.
		//
		// The request is finished by this call, so a second Take answers
		// nullptr. Holding the frame keeps its bytes alive even if the cache
		// evicts it - which is what lets a slow client finish a transfer while
		// the origin serves everybody else.
		//
		// @param id The request.
		// @return The frame, or nullptr when the request is not `Ready`.
		PreparedFrame Take(RequestId id);

		// How many requests have been accepted and not yet finished.
		size_t Outstanding() const;

		// The prepared-group cache, for diagnostics and for sharing.
		PreparedCache &Cache() {
			return Prepared;
		}

		// The settings in use, after the validity fallback.
		const CDNSettings &Settings() const {
			return Configured;
		}

	  private:
		struct Pending {
			engine::assets::ContentHash Bundle;
			std::shared_ptr<const Publication> Against;
			RequestState State = RequestState::Pending;
			PreparedFrame Frame;
		};

		// Resolves one bundle's bytes: local, upstream, or neither.
		std::optional<std::vector<std::byte>> Resolve(
			const engine::assets::ContentHash &bundle,
			const Publication &against,
			const PayloadSource &source,
			const UpstreamFetch &upstream,
			bool *fromUpstream = nullptr
		);

		// Whether bytes an upstream returned match what the local manifest says
		// that bundle weighs.
		bool
		Accepts(const engine::assets::ContentHash &bundle, const Publication &against, size_t bytes) const;

		Gate Admission;
		CDNSettings Configured;
		PreparedCache Prepared;
		std::shared_ptr<const Publication> Serving;
		std::vector<std::pair<uint64_t, Pending>> Requests;
		uint64_t NextRequest = 1;
	};
}
