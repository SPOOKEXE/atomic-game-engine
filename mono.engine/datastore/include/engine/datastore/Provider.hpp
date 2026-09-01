#pragma once

// Stable names for host-selectable DataStore providers.
//
// @tier L12 · shared

#include <optional>
#include <string_view>

namespace engine::datastore {
	// A built-in persistence adapter selectable by a host.
	enum class Provider {
		// Atomic snapshots in isolated local folders.
		File,
		// Complete snapshots addressed through plain HTTP GET and PUT.
		Http,
	};

	// Returns the stable configuration spelling of a provider.
	const char *Describe(Provider provider);
	// Parses a stable provider spelling.
	std::optional<Provider> ProviderOf(std::string_view text);
}
