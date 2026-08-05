#pragma once

// Where content is fetched from, in the order it is tried.
//
// CDN.md §6 gives content three sources — a local store, a server-proxied
// origin and a delivery service the client connects to — and one requirement
// that shapes this whole file: **the client must not know which one it is
// talking to.** So a source is a descriptor rather than a class, the list is
// ordered, and the fetch path walks it. Adding a fourth kind of place to get
// bytes from is a row in an enum rather than a second fetch path.
//
// **The order is the feature.** "Local cache first, then the origin next door,
// then the one across the internet" is not a policy the engine hard-codes — it
// is what a list in this order *means*, and it is what the studio's preferences
// edit. A deployment that wants the reverse writes the reverse.
//
// **Localhost is the default and that is a real decision.** A game being
// developed has its content beside it, and a default pointing anywhere else
// would make the first-run experience depend on a network. `repo_layout.md` §11
// wants moving from a self-hosted box to a delivery service to be a
// configuration change, and a default has to pick one of the two — so it picks
// the one that works with nothing else running.
//
// **A descriptor is untrusted.** It arrives in a game's session data, and
// `repo_layout.md` §11 is explicit: *a client that is told to fetch from an
// arbitrary host is a request-forgery primitive.* So a source list assembled
// from anything a server said is validated against the session's declared
// origins, and that check lives here rather than at the call sites — a call
// site added later is a call site that forgot.
//
// @tier L11 · shared

#include <engine/assets/Signature.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::delivery {

	// The port an origin listens on unless told otherwise.
	//
	// Chosen rather than derived, and deliberately not 80 or 8080: an origin
	// that squatted on a well-known port would collide with whatever else a
	// developer is running, and the failure reads as "the CDN is broken".
	constexpr uint16_t DEFAULT_ORIGIN_PORT = 9080;

	// What kind of place a source is.
	//
	// @since v0.9
	enum class SourceKind : uint8_t {
		// A directory on this machine holding a published content tree.
		//
		// **Not a shortcut around the format.** It reads the same manifest, the
		// same chunk store and the same signature an origin would serve, which
		// is what stops the configuration people develop against from being a
		// path that skips verification. `repo_layout.md` §16.6's argument about
		// the loopback transport, applied to content.
		Directory,

		// An origin reached over HTTP, on this machine or elsewhere.
		//
		// One kind for both, because "same machine" and "remote" differ in the
		// address and in nothing else. A separate kind for localhost would be a
		// second code path exercised only in development.
		Http,
	};

	// Returns a stable, human-readable name for a source kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(SourceKind kind);

	// One place content may be fetched from.
	//
	// @since v0.9
	struct Source {
		// What it is called.
		//
		// **Named as well as addressed**, for `AGENTS.md` rule 4's reason and
		// for a practical one: "source 2 timed out" is not something anybody
		// can act on, and this string is what a log line, the studio's
		// preferences list and a `--cdn` argument all carry.
		std::string Name;

		// What kind of place it is.
		SourceKind Kind = SourceKind::Http;

		// Where it is: a directory path for `Directory`, `host:port` for
		// `Http`.
		//
		// One field rather than a variant, because a source is a row in a
		// configuration file and a row has one address column.
		std::string Location;

		// Whether to try it.
		//
		// Kept in the list rather than removed, so the studio can turn an
		// origin off without losing how it was configured — which is what
		// somebody actually wants when they are working out which one is
		// broken.
		bool Enabled = true;

		// Whether this names somewhere usable.
		//
		// @return False for an empty name or location, or an `Http` location
		//         that is not `host:port`.
		bool IsValid() const;
	};

	// The whole of how a client fetches content.
	//
	// @since v0.9
	struct DeliverySettings {
		// The sources, tried in this order.
		//
		// First match wins, and a source that refuses or fails is passed over
		// rather than fatal — which is the entire point of there being a list.
		std::vector<Source> Sources;

		// Where verified content is kept between runs.
		//
		// **Consulted before any source**, and it is not itself a source: a
		// cache holds content this client has already verified against the
		// signed manifest, so reading from it is not a fetch and cannot be
		// pointed at somebody else's bytes. Empty disables it.
		std::filesystem::path CachePath;

		// What the cache may hold before it starts evicting.
		uint64_t CacheCapacityBytes = 1024ull * 1024ull * 1024ull;

		// The key that signs manifests — this client's root of trust.
		//
		// **Without it nothing verifies and nothing is fetched.** An unset key
		// is refused at validation rather than treated as "trust anything":
		// a delivery client that accepts an unsigned manifest is a delivery
		// client with no trust boundary at all, and that failure is invisible
		// until somebody is serving you content you did not publish.
		assets::PublicKey Publisher;

		// Hosts an `Http` source is allowed to name.
		//
		// The request-forgery check, in one place. Empty means "no restriction"
		// and is right for a source list a person typed into their own
		// preferences; a list assembled from session data a server sent must
		// fill this in, which is what `Validate` is for.
		std::vector<std::string> AllowedHosts;

		// How many polls a fetch may go without progress before that source is
		// given up on and the next is tried.
		uint32_t IdlePolls = 6000;

		// The settings a client starts with: one origin, on this machine.
		//
		// @param cachePath Where to keep verified content.
		// @return Settings naming `127.0.0.1:9080` and nothing else.
		static DeliverySettings Default(const std::filesystem::path &cachePath = {});

		// Whether these can be used.
		//
		// @return False with no usable source, with no publisher key, or when
		//         a source names a host `AllowedHosts` does not permit.
		bool IsValid() const;

		// The sources that are enabled and valid, in priority order.
		//
		// @return The list to walk.
		std::vector<Source> Usable() const;
	};

	// Whether a location names a host this settings block permits.
	//
	// Exposed because the studio wants to say *why* a row is refused while
	// somebody is typing it, and re-implementing the rule there would be the
	// second copy of the check that eventually differs.
	//
	// @param location The `host:port` to check.
	// @param allowed The permitted hosts, or empty for no restriction.
	// @return Whether a fetch may be made to it.
	bool HostPermitted(std::string_view location, const std::vector<std::string> &allowed);
}
