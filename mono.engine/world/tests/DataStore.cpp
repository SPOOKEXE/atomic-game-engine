#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_SUITE_ID("engine.world.datastore")
TEST_DEPENDS("engine.world.sharedstorefile")

namespace {
	using engine::core::Name;
	using engine::world::DataStoreAdapter;
	using engine::world::DataStoreStatus;
	using engine::world::SharedStoreEntry;

	struct RecordingAdapter final : DataStoreAdapter {
		Name Loaded;
		Name Saved;
		std::vector<SharedStoreEntry> Contents;

		DataStoreStatus
		Load(Name store, std::vector<SharedStoreEntry> &entries, std::string &error) override {
			Loaded = store;
			entries = Contents;
			error.clear();
			return DataStoreStatus::Ok;
		}

		DataStoreStatus
		Save(Name store, std::span<const SharedStoreEntry> entries, std::string &error) override {
			Saved = store;
			Contents.assign(entries.begin(), entries.end());
			error.clear();
			return DataStoreStatus::Ok;
		}
	};
}

TEST_CASE("datastore router assigns each store to its selected adapter", "[world][datastore]") {
	engine::world::DataStoreRouter router;
	auto first = std::make_unique<RecordingAdapter>();
	RecordingAdapter *firstView = first.get();
	auto second = std::make_unique<RecordingAdapter>();
	RecordingAdapter *secondView = second.get();

	REQUIRE(router.AddAdapter(Name("local"), std::move(first)));
	REQUIRE(router.AddAdapter(Name("remote"), std::move(second)));
	CHECK_FALSE(router.AddAdapter(Name("local"), std::make_unique<RecordingAdapter>()));
	REQUIRE(router.Assign(Name("players"), Name("local")));
	REQUIRE(router.Assign(Name("economy"), Name("remote")));
	CHECK_FALSE(router.Assign(Name("missing"), Name("not-registered")));

	std::vector<SharedStoreEntry> entries;
	std::string error;
	CHECK(router.Load(Name("players"), entries, error) == DataStoreStatus::Ok);
	CHECK(firstView->Loaded == Name("players"));
	CHECK_FALSE(secondView->Loaded.IsValid());
	CHECK(router.Save(Name("economy"), entries, error) == DataStoreStatus::Ok);
	CHECK(secondView->Saved == Name("economy"));
	CHECK(router.AdapterFor(Name("players")) == Name("local"));

	REQUIRE(router.Unassign(Name("players")));
	CHECK_FALSE(router.Unassign(Name("players")));
	CHECK(router.Load(Name("players"), entries, error) == DataStoreStatus::NoRoute);
	CHECK_FALSE(error.empty());
}

TEST_CASE("file datastore adapter preserves default paths and isolates names", "[world][datastore]") {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / "atomic-datastore-adapter-test";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);

	const Name defaultStore(engine::world::DEFAULT_DATASTORE);
	CHECK(
		engine::world::DataStoreFilePath(root, engine::world::SharedStoreEnvironment::Mock, defaultStore) ==
		root / "mock" / "datastore.bin"
	);
	const std::filesystem::path named = engine::world::DataStoreFilePath(
		root, engine::world::SharedStoreEnvironment::Live, Name("../players")
	);
	CHECK(named.parent_path() == root / "live" / "datastores");
	CHECK(named.filename() == "2e2e2f706c6179657273.bin");

	engine::world::DataStoreRouter router;
	REQUIRE(router.AddAdapter(
		Name(engine::world::FILE_DATASTORE_ADAPTER),
		engine::world::MakeFileDataStoreAdapter(root, engine::world::SharedStoreEnvironment::Mock)
	));
	REQUIRE(router.Assign(defaultStore, Name(engine::world::FILE_DATASTORE_ADAPTER)));

	SharedStoreEntry entry;
	entry.Store = engine::world::BusKind::DataStore;
	entry.Key = Name("score");
	entry.Value = {std::byte{4}, std::byte{2}};
	entry.Version = 1;
	const std::vector<SharedStoreEntry> saved{entry};
	std::string error;
	REQUIRE(router.Save(defaultStore, saved, error) == DataStoreStatus::Ok);

	std::vector<SharedStoreEntry> loaded;
	REQUIRE(router.Load(defaultStore, loaded, error) == DataStoreStatus::Ok);
	CHECK(loaded == saved);

	std::filesystem::remove_all(root, ignored);
}

TEST_CASE("datastore router refuses invalid names and malformed snapshots", "[world][datastore]") {
	engine::world::DataStoreRouter router;
	CHECK_FALSE(router.AddAdapter(Name{}, std::make_unique<RecordingAdapter>()));
	CHECK_FALSE(router.AddAdapter(Name("null"), nullptr));

	const std::string overlong(engine::world::MAXIMUM_DATASTORE_NAME_BYTES + 1, 'x');
	REQUIRE(router.AddAdapter(
		Name(engine::world::FILE_DATASTORE_ADAPTER),
		engine::world::MakeFileDataStoreAdapter({}, engine::world::SharedStoreEnvironment::Live)
	));
	CHECK_FALSE(router.Assign(Name(overlong), Name(engine::world::FILE_DATASTORE_ADAPTER)));

	REQUIRE(
		router.Assign(Name(engine::world::DEFAULT_DATASTORE), Name(engine::world::FILE_DATASTORE_ADAPTER))
	);
	SharedStoreEntry malformed;
	malformed.Store = engine::world::BusKind::MemoryStore;
	malformed.Key = Name("wrong-store");
	std::string error;
	CHECK(
		router.Save(
			Name(engine::world::DEFAULT_DATASTORE), std::span<const SharedStoreEntry>(&malformed, 1), error
		) == DataStoreStatus::Malformed
	);
	CHECK_FALSE(error.empty());
}
