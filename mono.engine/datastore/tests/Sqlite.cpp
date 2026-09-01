#include <engine/datastore/Sqlite.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

TEST_SUITE_ID("engine.datastore.sqlite")
TEST_DEPENDS("engine.world.sharedstorefile")

namespace {
	using engine::core::Name;
	using engine::world::BusKind;
	using engine::world::DataStoreStatus;
	using engine::world::SharedStoreEntry;

	std::filesystem::path Scratch() {
		const std::filesystem::path root =
			std::filesystem::temp_directory_path() / "atomic-sqlite-datastore-test";
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
		return root;
	}
}

TEST_CASE("SQLite datastore saves, loads, and atomically replaces an image", "[datastore][sqlite]") {
	const std::filesystem::path root = Scratch();
	auto adapter = engine::datastore::MakeSqliteDataStoreAdapter(
		root, engine::world::SharedStoreEnvironment::Live
	);
	const Name store("players");
	std::vector<SharedStoreEntry> loaded;
	std::string error;
	CHECK(adapter->Load(store, loaded, error) == DataStoreStatus::NotFound);

	const std::vector<SharedStoreEntry> first{
		{BusKind::DataStore, Name("score"), {std::byte{4}, std::byte{2}}, 1},
	};
	REQUIRE(adapter->Save(store, first, error) == DataStoreStatus::Ok);
	REQUIRE(adapter->Load(store, loaded, error) == DataStoreStatus::Ok);
	CHECK(loaded == first);
	CHECK(
		engine::datastore::SqliteDataStorePath(root, engine::world::SharedStoreEnvironment::Live) ==
		root / "live" / "datastores.sqlite3"
	);

	const std::vector<SharedStoreEntry> second{
		{BusKind::DataStore, Name("level"), {std::byte{9}}, 7},
	};
	REQUIRE(adapter->Save(store, second, error) == DataStoreStatus::Ok);
	REQUIRE(adapter->Load(store, loaded, error) == DataStoreStatus::Ok);
	CHECK(loaded == second);

	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
}

TEST_CASE("SQLite datastore leaves the caller unchanged on malformed input", "[datastore][sqlite]") {
	const std::filesystem::path root = Scratch();
	auto adapter = engine::datastore::MakeSqliteDataStoreAdapter(
		root, engine::world::SharedStoreEnvironment::Mock
	);
	const std::vector<SharedStoreEntry> malformed{
		{BusKind::MemoryStore, Name("wrong"), {}, 0},
	};
	std::string error;
	CHECK(adapter->Save(Name("main"), malformed, error) == DataStoreStatus::Malformed);
	CHECK_FALSE(error.empty());
	CHECK_FALSE(std::filesystem::exists(
		engine::datastore::SqliteDataStorePath(root, engine::world::SharedStoreEnvironment::Mock)
	));
}
