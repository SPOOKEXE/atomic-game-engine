#pragma once

// Stable names for the file format used by a local DataStore provider.
//
// A provider answers where snapshots travel. A backend answers which durable
// file format holds them. Keeping the two apart lets HTTP remain HTTP while a
// local provider can choose a portable binary image or a real SQLite database.
//
// @tier L12 · shared

#include <optional>
#include <string_view>

namespace engine::datastore {
	// A local durable DataStore file format.
	enum class Backend {
		// The engine's compact atomic snapshot format.
		Binary,
		// A SQLite database containing atomic portable snapshots.
		SQLite,
	};

	// Returns the stable configuration spelling of a backend.
	const char *Describe(Backend backend);

	// Parses a stable backend spelling.
	std::optional<Backend> BackendOf(std::string_view text);
}
