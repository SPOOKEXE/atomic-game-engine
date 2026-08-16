#pragma once

// Content routes carried over a connection this module does not own.
//
// **Two halves of one arrangement, and they are deliberately in one file.** A
// relay is a route asked for at one end and answered at the other; splitting
// the asking from the answering into two headers is how the two ends acquire a
// dialect, which is the mistake `CDN.md` §6 refuses twice for the manifest.
//
// - `RelayChannel` and `MakeRelayClient` are the **asking** half. A client with
//   no origin connection at all asks for the same three routes an origin
//   serves, over a link somebody else owns, and everything downstream is the
//   ordinary fetch path: the manifest is verified against the publisher key and
//   every asset against the signed root. **Relaying moves bytes and does not
//   move the trust boundary.**
// - `RouteFetcher` is the **answering** half. It walks a `DeliverySettings`
//   source list and produces a route's bytes exactly as an origin would have,
//   so whoever holds the link can hand them on without learning what a manifest
//   is.
//
// **The routes are the origin's, not a second protocol.** `/manifest`,
// `/dictionary` and `/bundle/<hex>` are what `cdn::Service` answers, and a
// relayed request is the same string. A relay that invented its own verbs would
// be a fourth description of what an origin serves.
//
// @tier L11 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/net/http/Client.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::delivery {

	// The catalogue route.
	inline constexpr std::string_view MANIFEST_ROUTE = "/manifest";

	// The compression dictionary route.
	inline constexpr std::string_view DICTIONARY_ROUTE = "/dictionary";

	// What every bundle route starts with.
	inline constexpr std::string_view BUNDLE_ROUTE_PREFIX = "/bundle/";

	// The route naming one bundle.
	//
	// @param bundle The bundle root.
	// @return The route an origin serves it at.
	// @since v0.16
	std::string BundleRoute(const assets::ContentHash &bundle);

	// The bundle a route names.
	//
	// @param route The route, which is untrusted text.
	// @return The bundle root, or nothing when the route names no bundle.
	// @since v0.16
	std::optional<assets::ContentHash> BundleOfRoute(std::string_view route);

	// Whether a route is one of the three a relay carries.
	//
	// **The whole of what a relay will ask for or answer.** A route is a string
	// that arrived over a wire, so the check is a closed list rather than a
	// prefix test - an open one would let a peer name `/ingest/` or `/catalogue`
	// through a hop that was never meant to carry either.
	//
	// @param route The route.
	// @return Whether it may be relayed.
	// @since v0.16
	bool RelayableRoute(std::string_view route);

	// --- asking: the half a client with no origin connection uses -----------

	// One route a relay answered.
	//
	// @since v0.16
	struct RelayAnswer {
		// The ticket the request was made under.
		uint64_t Ticket = 0;

		// Whether the far end had the route at all.
		//
		// **A boolean rather than a reason**, which is `cdn::Gate`'s rule
		// arriving one hop later: a caller falls through to the next source
		// either way, and a taxonomy handed back over a wire is an oracle.
		bool Served = false;

		// The bytes, when it was served.
		std::vector<std::byte> Bytes;
	};

	// Whoever owns the connection a relayed route travels over.
	//
	// **A seam, for `cdn::PayloadSource`'s reason.** The connection is a game
	// link at L12 and this module is at L11, so the transport cannot be named
	// here and must not be: a delivery client that knew what a replication
	// session was would be a delivery client that only one program can build.
	//
	// @since v0.16
	class RelayChannel {
	  public:
		virtual ~RelayChannel() = default;

		// Asks the far end for one route.
		//
		// @param ticket What the answer will be named by.
		// @param route  The route to ask for.
		// @return `false` when the link would not take it, which is ordinary
		//         backpressure: the request is offered again on a later pump.
		virtual bool Ask(uint64_t ticket, std::string_view route) = 0;

		// Takes whatever has arrived since the last call.
		//
		// @param into Appended to. Never cleared, so a caller may accumulate.
		virtual void Collect(std::vector<RelayAnswer> &into) = 0;

		// Says a ticket will never be taken.
		//
		// @param ticket The abandoned request.
		virtual void Abandon(uint64_t ticket) = 0;
	};

	// Builds a fetching client that goes through a relay rather than a socket.
	//
	// **It is a `net::http::Client` because that is already the seam.** The
	// delivery client submits, pumps, takes and cancels against that interface
	// for every HTTP source it has, so a relay wearing the same shape reuses the
	// whole fetch path - the manifest walk, the dictionary, the bundle jobs, the
	// three verification checks - rather than growing a second copy of it beside
	// the first.
	//
	// The endpoint and host a caller submits with are ignored: a relay is
	// reached through `channel` and has no address.
	//
	// @param channel Who carries the request. Borrowed for the client's life.
	// @param idlePolls How many pumps a relayed request may go without an
	//        answer before it fails and the next source is tried. Counted in
	//        polls rather than wall time, `net/http`'s rule, which is what lets
	//        a suite state a timeout instead of sleeping for one. Zero disables
	//        it, and a relay whose owner never answers then waits for ever.
	// @return The client. Never null.
	// @since v0.16
	std::unique_ptr<net::http::Client> MakeRelayClient(RelayChannel &channel, uint32_t idlePolls = 6000);

	// --- answering: the half whoever owns the link uses ---------------------

	// Where a route request has got to.
	//
	// @since v0.16
	enum class RouteState : uint8_t {
		// Not a request this fetcher issued, or one already taken.
		Unknown,

		// Waiting on a source.
		Pending,

		// The bytes are here.
		Ready,

		// Every source was tried and none produced the route.
		Refused,
	};

	// Returns a stable, human-readable name for a route state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(RouteState state);

	// What a fetcher has done, for a diagnostic and for a test.
	//
	// @since v0.16
	struct RouteCounters {
		// Routes answered with bytes.
		uint64_t Served = 0;

		// Routes no source produced.
		uint64_t Refused = 0;

		// Times a source was passed over and the next one tried.
		uint64_t SourceFailures = 0;

		// Manifests a source served that did not verify against `Publisher`.
		//
		// **Counted apart from an ordinary failure**, `assets::Grant`'s rule: a
		// source that is down is an operational event and one serving bytes that
		// do not match a signed root is corruption or an attack.
		uint64_t VerificationFailures = 0;

		// Route bytes handed back.
		uint64_t ServedBytes = 0;
	};

	// Produces a route's bytes out of a source list.
	//
	// **This is not a second origin.** It reads a store through
	// `assets::ChunkStore`, compresses a group through `GroupCodec` and forwards
	// an HTTP route verbatim - one implementation of each, all of them already
	// here. What it does not do is decide who may have content: a grant is
	// issued by a server and opened by an origin, and this carries one.
	//
	// **One owner, one thread**, like every other pump in this module.
	//
	// @since v0.16
	class RouteFetcher {
	  public:
		virtual ~RouteFetcher() = default;

		// Hands over the grant to present to an HTTP source.
		//
		// @param token The bytes a server issued.
		virtual void UseGrant(std::span<const std::byte> token) = 0;

		// Asks for one route.
		//
		// @param route The route. Anything `RelayableRoute` refuses is refused
		//        here, so a caller may pass a string that arrived over a wire.
		// @return A ticket, or zero when the route is not one this carries.
		virtual uint64_t Request(std::string_view route) = 0;

		// Drives every outstanding request.
		//
		// @return How many reached `Ready` or `Refused`.
		virtual size_t Pump() = 0;

		// Where a request has got to.
		//
		// @param ticket The request.
		// @return Its state.
		virtual RouteState StateOf(uint64_t ticket) const = 0;

		// Takes a finished request's bytes.
		//
		// The request is finished by this call whether it was served or refused,
		// so a second take answers `Unknown`.
		//
		// @param ticket The request.
		// @param[out] bytes Filled when the route was served.
		// @return `false` when the request is not finished, which a caller
		//         distinguishes from a refusal by asking `StateOf` first.
		virtual bool Take(uint64_t ticket, std::vector<std::byte> &bytes) = 0;

		// Abandons a request.
		//
		// @param ticket The request to abandon.
		// @return `false` for an unknown request.
		virtual bool Cancel(uint64_t ticket) = 0;

		// How many requests are outstanding.
		virtual size_t Outstanding() const = 0;

		// What this fetcher has done.
		virtual const RouteCounters &Counters() const = 0;
	};

	// How a route fetcher is sized and bounded.
	//
	// @since v0.16
	struct RouteFetcherSettings {
		// The level a `Directory` source's groups are compressed at.
		//
		// **A directory holds chunks and a relay carries frames**, so this end
		// is where the compression happens for a local store - the same place
		// `cdn::Origin` does it, at the same default, because a relayed group
		// and a served one are the same artefact.
		int CompressionLevel = GroupCodec::DEFAULT_LEVEL;

		// How many requests may be outstanding at once.
		//
		// Bounded rather than unlimited: each one holds a group in memory, and
		// the peer asking is not trusted to be reasonable about how many.
		size_t MaximumOutstanding = 8;

		// How many polls a request may go without progress before it is refused.
		uint32_t IdlePolls = 6000;
	};

	// Builds a route fetcher over a source list.
	//
	// @param settings Where to fetch from. `Publisher` is optional here and is
	//        used for the length of one check: a relayed manifest that does not
	//        verify against it is passed over rather than forwarded. A zero key
	//        skips that check, which is the honest state of a relay whose
	//        operator was never given the publisher's key - the client verifies
	//        end to end regardless, so this is defence in depth.
	// @param limits How to size and bound it.
	// @return The fetcher, or nothing when the settings name no usable source.
	// @since v0.16
	std::unique_ptr<RouteFetcher>
	MakeRouteFetcher(const DeliverySettings &settings, const RouteFetcherSettings &limits = {});
}
