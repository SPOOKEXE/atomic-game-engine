#pragma once

// Asking for content by name or by kind, and getting bytes back - the client
// half of delivery.
//
// **The completion becomes visible when the caller pumps, and at no other
// moment.** That single sentence is why this class exists in the shape it does.
// A fetch issued from a world lives inside a tick, and `AGENTS.md` rule 5 has no
// exception there: a chunk that becomes visible to a system mid-tick is a
// desync, because two machines whose networks happened to differ would then
// simulate different things. So there is no callback, no future and no
// background thread that could deliver at a moment scheduling decided. There is
// `Pump`, and a world calls it at the barrier.
//
// **The client trusts the manifest, not the origin.** Everything
// that arrives is verified against a root the publisher signed, so an origin
// that is compromised, misconfigured or stale can withhold content but cannot
// substitute it. That property is what makes it safe to fetch from a delivery
// service somebody else runs, and it is checked here rather than assumed:
//
// - the manifest's signature, against `DeliverySettings::Publisher`
// - every chunk of every asset, against the asset root
// - the asset root, against the bundle root the manifest signed
//
// **Sources are walked in order and a failure falls through to the next.** The
// cache first - content already verified - then each enabled source. A source
// that refuses, times out or serves something that does not verify is passed
// over, and only when every one has been tried does a request fail. That is
// what makes "local first, then the origin next door, then the one across the
// internet" a configuration rather than code.
//
// **The unit that travels is a group, and the unit asked for is an asset.**
// A group is the thing that is compressed, streamed and cancelled,
// because per-asset requests are thousands of round trips. So asking for one
// asset fetches the bundle that carries it, and the other assets in that
// bundle land in the cache as a consequence - which is the whole of "the game
// progressively builds" seen from this end.
//
// @tier L11 · shared

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/delivery/Cache.hpp>
#include <engine/delivery/Source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::delivery {

	class RelayChannel;

	// One delivered asset.
	//
	// @since v0.9
	struct Asset {
		// What it was asked for by.
		std::string Name;

		// What subsystem it belongs to, from the signed manifest.
		assets::AssetKind Kind = assets::AssetKind::Unknown;

		// Its content address.
		assets::ContentHash Root;

		// The bytes, verified against `Root`.
		//
		// **Opaque here.** This module delivers and verifies; what is *inside*
		// a mesh or a sound is the importer's and the subsystem's, and there is
		// no format in the engine yet for either. A delivery layer that parsed
		// content would be a second place that decides what an asset is.
		std::vector<std::byte> Bytes;
	};

	// A request in flight.
	//
	// A number rather than a pointer, for `AGENTS.md` rule 3's reason.
	//
	// @since v0.9
	struct RequestId {
		// The value meaning "no request". Zero is never issued.
		static constexpr uint64_t NONE = 0;

		// The client's monotonic counter.
		uint64_t Value = NONE;

		// Whether this names a request at all.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// Whether two handles name the same request.
		constexpr bool operator==(const RequestId &other) const = default;
	};

	// Where a request has got to.
	//
	// @since v0.9
	enum class RequestState : uint8_t {
		// Not a request this client issued, or one already taken.
		Unknown,

		// Waiting on the manifest, a source, or verification.
		Pending,

		// The bytes are here and verified.
		Ready,

		// Every source was tried and none produced content that verified.
		//
		// **One state rather than a taxonomy.** A caller retries, falls back to
		// a placeholder, or gives up either way, and the reason belongs in the
		// log and the counters rather than in a value every call site has to
		// switch on.
		Failed,
	};

	// Returns a stable, human-readable name for a request state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(RequestState state);

	// What a client has done, for a diagnostic and for a test.
	//
	// @since v0.9
	struct DeliveryCounters {
		// Assets served without touching a source.
		uint64_t CacheHits = 0;

		// Assets that had to be fetched.
		uint64_t CacheMisses = 0;

		// Bytes that actually crossed a socket or came off a disk, **as they
		// travelled** - compressed.
		uint64_t TransferredBytes = 0;

		// Bytes those became once expanded.
		//
		// The pair is what answers "did this travel compressed", which is a
		// question about the wire and therefore has to be measured at it
		// rather than inferred from a setting.
		uint64_t ExpandedBytes = 0;

		// Bundles fetched.
		uint64_t Bundles = 0;

		// Times a source was passed over and the next one tried.
		uint64_t SourceFailures = 0;

		// Payloads that arrived and did not verify against the signed manifest.
		//
		// **Counted apart from an ordinary failure.** A source that is down is
		// an operational event; a source serving bytes that do not match a
		// signed root is either corruption or an attack, and one counter for
		// both buries the second in the first - `assets::Grant`'s rule.
		uint64_t VerificationFailures = 0;
	};

	// Fetches assets from a prioritised list of sources.
	//
	// **One owner, one thread.**
	//
	// @since v0.9
	class AssetClient {
	  public:
		virtual ~AssetClient() = default;

		// Hands over the grant a server issued.
		//
		// The origin admits a request against this and nothing else - it holds
		// no accounts and no sessions, because an origin that knew players is a
		// second authority. A client with no grant can still read a `Directory`
		// source, because there is no origin in that path to admit anything.
		//
		// @param token The bytes the server sent.
		virtual void UseGrant(std::span<const std::byte> token) = 0;

		// Whether a verified manifest is in hand.
		//
		// Until it is, requests are accepted and wait: a caller should not have
		// to sequence its own start-up around a network fetch, and a request
		// made too early is the most ordinary thing that could happen.
		virtual bool Ready() const = 0;

		// What this client knows it can fetch.
		//
		// @return The verified manifest, or nullptr before one has arrived.
		virtual const assets::Manifest *Catalogue() const = 0;

		// The signature that authenticated the current catalogue.
		//
		// Kept beside `Catalogue` so an exporter can ground the verified manifest
		// without owning the publisher's private key or changing its identity.
		//
		// @return The signature, or nullptr before a catalogue has arrived.
		// @since v0.21
		virtual const assets::SignatureBytes *CatalogueSignature() const = 0;

		// Asks for an asset by the name a game author wrote.
		//
		// @param name The content name.
		// @return A handle. Always valid; ask `StateOf` what happened. A name
		//         the manifest does not describe fails on the first pump rather
		//         than being refused here, so that a request made before the
		//         manifest arrived behaves the same as one made after.
		virtual RequestId Request(std::string_view name) = 0;

		// Asks for an asset by its content address.
		//
		// @param root The asset root.
		// @return A handle.
		virtual RequestId RequestRoot(const assets::ContentHash &root) = 0;

		// Asks for every asset of one kind.
		//
		// What "connect and get assets of types" is at this layer. Requires a
		// manifest, since the kinds are in it - an empty result before `Ready`
		// means "not yet" rather than "none".
		//
		// @param kind The kind to fetch.
		// @return One handle per matching asset, in name order.
		virtual std::vector<RequestId> RequestKind(assets::AssetKind kind) = 0;

		// Drives everything: the manifest, the sources, verification.
		//
		// @return How many requests reached `Ready` or `Failed`.
		virtual size_t Pump() = 0;

		// Where a request has got to.
		//
		// @param id The request.
		// @return Its state.
		virtual RequestState StateOf(RequestId id) const = 0;

		// Takes a ready request's asset.
		//
		// @param id The request.
		// @return The asset, or nothing when it is not `Ready`.
		virtual std::optional<Asset> Take(RequestId id) = 0;

		// What a request asked for, by name.
		//
		// **Because a failure does not carry one and a caller needs it.**
		// `Take` answers with an `Asset` and an `Asset` has a `Name`, so a
		// request that succeeded tells you what it was; one that failed answers
		// nothing at all. A caller that has to undo something it did at request
		// time - mark a texture as expected, count a fetch, show a row - could
		// only do it by keeping its own handle-to-name map, which is bookkeeping
		// duplicated in every host and wrong in the one that forgets.
		//
		// Empty for a request by content address, which named nothing, and for
		// one that has been taken or cancelled.
		//
		// **A view into the client**, valid until the next call that changes its
		// request list - `Pump`, `Take`, `Cancel` or another `Request`. A caller
		// keeping it across one of those is keeping a dangling view.
		//
		// @param id The request.
		// @return The name it was asked for by, or empty.
		// @since v0.13
		virtual std::string_view NameOf(RequestId id) const = 0;

		// Abandons a request.
		//
		// @param id The request to abandon.
		// @return False for an unknown request or one already finished.
		virtual bool Cancel(RequestId id) = 0;

		// How many requests have been made and not yet taken.
		virtual size_t Outstanding() const = 0;

		// What this client has done.
		virtual const DeliveryCounters &Counters() const = 0;

		// The settings in use.
		virtual const DeliverySettings &Settings() const = 0;
	};

	// Builds a delivery client.
	//
	// @param settings Where to fetch from and what to trust. Invalid settings
	//        are refused rather than half-applied - a client with no publisher
	//        key would verify nothing, and one with no source would fetch
	//        nothing, and neither should look like a working client.
	// @param relay Who carries a `SourceKind::Relay` source's routes, or null
	//        when this caller owns no such link. A relay source with nothing to
	//        carry it is skipped with one warning rather than refusing the whole
	//        client, because the rest of the list is still perfectly good.
	// @return The client, or nothing when the settings are not usable.
	// @since v0.9
	std::unique_ptr<AssetClient>
	MakeAssetClient(const DeliverySettings &settings, RelayChannel *relay = nullptr);
}
