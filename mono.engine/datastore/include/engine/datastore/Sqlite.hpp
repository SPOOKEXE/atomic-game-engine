#pragma once

// SQLite-backed durable DataStore snapshots.
//
// The live rows remain in `world`. This adapter stores one complete portable
// image per logical datastore, so validation stays the shared-store format's
// and SQLite supplies the file format and atomic row replacement.
//
// @tier L12 · shared

#include <engine/world/DataStore.hpp>

#include <filesystem>
#include <memory>

namespace engine::datastore {
	// The stable router name for the SQLite adapter.
	inline constexpr std::string_view SQLITE_DATASTORE_ADAPTER = "sqlite";

	// Resolves the SQLite database shared by one environment's logical stores.
	std::filesystem::path
	SqliteDataStorePath(const std::filesystem::path &root, world::SharedStoreEnvironment environment);

	// Builds a SQLite adapter for one root and environment.
	std::unique_ptr<world::DataStoreAdapter>
	MakeSqliteDataStoreAdapter(std::filesystem::path root, world::SharedStoreEnvironment environment);
}
