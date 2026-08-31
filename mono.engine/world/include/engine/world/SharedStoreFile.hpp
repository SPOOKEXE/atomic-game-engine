#pragma once

// A bounded, portable persistence image for one shared key/value store.
//
// The file is an adapter boundary, not another store. It contains copied keys,
// values and DataStore versions, and `Universe::ReplaceSharedStoreEntries`
// installs a successfully decoded image in one operation.
//
// @tier L4 · shared

#include <engine/world/SharedStores.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace engine::world {

	enum class SharedStoreFileStatus {
		Ok,
		NotFound,
		IoError,
		Malformed,
		WrongStore,
	};

	const char *Describe(SharedStoreFileStatus status);

	// Atomically writes one MemoryStore or DataStore image.
	SharedStoreFileStatus SaveSharedStoreFile(
		const std::filesystem::path &path,
		BusKind store,
		std::span<const SharedStoreEntry> entries,
		std::string &error
	);

	// Reads and validates one complete image, replacing `entries` only on success.
	SharedStoreFileStatus LoadSharedStoreFile(
		const std::filesystem::path &path,
		BusKind expectedStore,
		std::vector<SharedStoreEntry> &entries,
		std::string &error
	);
}
