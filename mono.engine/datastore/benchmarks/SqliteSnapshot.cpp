// Measures the durable provider against a complete shared-store snapshot.

#include <engine/core/Name.hpp>
#include <engine/datastore/Sqlite.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/DataStore.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.datastore.bench.sqlite-snapshot")

namespace {
	using engine::core::Name;
	using engine::datastore::MakeSqliteDataStoreAdapter;
	using engine::world::BusKind;
	using engine::world::DataStoreStatus;
	using engine::world::SharedStoreEntry;

	constexpr size_t ENTRY_COUNT = 1024;
	constexpr size_t VALUE_BYTES = 256;

	struct SnapshotFixture {
		std::filesystem::path Root =
			std::filesystem::current_path() / ".cache/build/bench/benchmark-data/datastore-sqlite";
		std::unique_ptr<engine::world::DataStoreAdapter> Adapter;
		std::vector<SharedStoreEntry> Entries;
		std::vector<SharedStoreEntry> Loaded;
		std::string Error;

		SnapshotFixture() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
			Entries.reserve(ENTRY_COUNT);
			for (size_t index = 0; index < ENTRY_COUNT; ++index) {
				std::vector<std::byte> value(VALUE_BYTES, static_cast<std::byte>(index & 0xffu));
				Entries.push_back(
					{BusKind::DataStore, Name("entry." + std::to_string(index)), std::move(value), index + 1}
				);
			}
			Adapter = MakeSqliteDataStoreAdapter(Root, engine::world::SharedStoreEnvironment::Live);
			if (Adapter->Save(Name("main"), Entries, Error) != DataStoreStatus::Ok) {
				throw std::runtime_error("could not seed SQLite benchmark snapshot: " + Error);
			}
		}

		~SnapshotFixture() {
			Adapter.reset();
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}

		void Save() {
			if (Adapter->Save(Name("main"), Entries, Error) != DataStoreStatus::Ok) {
				throw std::runtime_error("SQLite benchmark save failed: " + Error);
			}
		}

		void Load() {
			if (Adapter->Load(Name("main"), Loaded, Error) != DataStoreStatus::Ok ||
				Loaded.size() != ENTRY_COUNT) {
				throw std::runtime_error("SQLite benchmark load failed: " + Error);
			}
		}
	};

	SnapshotFixture &Fixture() {
		static SnapshotFixture fixture;
		return fixture;
	}
}

BENCH("SQLite atomic snapshot replace, 1024 entries x 256 bytes", 1) {
	Fixture().Save();
}

BENCH("SQLite snapshot load, 1024 entries x 256 bytes", 1) {
	Fixture().Load();
	engine::testing::Consume(Fixture().Loaded.size());
}
