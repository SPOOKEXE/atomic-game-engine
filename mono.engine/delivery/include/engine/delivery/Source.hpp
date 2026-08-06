#pragma once

// Content sources are tried in list order. Source descriptors are untrusted and
// HTTP hosts must pass the configured allow-list.
//
// @tier L11 · shared

#include <engine/assets/Signature.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::delivery {

	// Default development origin port.
	constexpr uint16_t DEFAULT_ORIGIN_PORT = 9080;

	// What kind of place a source is.
	//
	// @since v0.9
	enum class SourceKind : uint8_t {
		// A local published content tree.
		Directory,

		// An HTTP origin.
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
		// Display name used by logs and preferences.
		std::string Name;

		// Source type.
		SourceKind Kind = SourceKind::Http;

		// Directory path or `host:port`.
		std::string Location;

		// Whether the client tries this source.
		bool Enabled = true;

		// Returns false for an empty name or location, or an invalid HTTP location.
		bool IsValid() const;
	};

	// The whole of how a client fetches content.
	//
	// @since v0.9
	struct DeliverySettings {
		// Sources are tried in this order. Failed sources are skipped.
		std::vector<Source> Sources;

		// Cache for verified content. Empty disables the cache.
		std::filesystem::path CachePath;

		// What the cache may hold before it starts evicting.
		uint64_t CacheCapacityBytes = 1024ull * 1024ull * 1024ull;

		// Publisher key for manifest verification. Required for valid settings.
		assets::PublicKey Publisher;

		// Allowed HTTP hosts. Empty allows all hosts.
		std::vector<std::string> AllowedHosts;

		// Polls without progress before the client tries the next source.
		uint32_t IdlePolls = 6000;

		// The settings a client starts with: one origin, on this machine.
		//
		// @param cachePath Where to keep verified content.
		// @return Settings naming `127.0.0.1:9080` and nothing else.
		static DeliverySettings Default(const std::filesystem::path &cachePath = {});

		// Returns false without a usable source, publisher key, or permitted host.
		bool IsValid() const;

		// Returns enabled and valid sources in priority order.
		std::vector<Source> Usable() const;
	};

	// Returns whether an HTTP location uses an allowed host.
	//
	// @param location The `host:port` to check.
	// @param allowed The permitted hosts, or empty for no restriction.
	// @return Whether a fetch may be made to it.
	bool HostPermitted(std::string_view location, const std::vector<std::string> &allowed);
}
