#include <engine/testing/Suite.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.sharedstorefile")

using engine::core::Name;
using engine::world::BusKind;
using engine::world::LoadSharedStoreFile;
using engine::world::SaveSharedStoreFile;
using engine::world::SharedStoreEntry;
using engine::world::SharedStoreFileStatus;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		const auto *first = reinterpret_cast<const std::byte *>(text.data());
		return {first, first + text.size()};
	}

	struct TemporaryFile {
		explicit TemporaryFile(const char *name) : Path(std::filesystem::temp_directory_path() / name) {
			std::error_code ignored;
			std::filesystem::remove(Path, ignored);
			std::filesystem::remove(Path.string() + ".tmp", ignored);
		}

		~TemporaryFile() {
			std::error_code ignored;
			std::filesystem::remove(Path, ignored);
			std::filesystem::remove(Path.string() + ".tmp", ignored);
		}

		std::filesystem::path Path;
	};
}

TEST_CASE("a datastore file round-trips values and versions", "[world][shared-store-file]") {
	TemporaryFile file("atomic-shared-store-roundtrip.bin");
	const std::vector<SharedStoreEntry> written{
		{BusKind::DataStore, Name("z"), Bytes("last"), 7},
		{BusKind::DataStore, Name("a"), Bytes(std::string_view("a\0b", 3)), 2},
	};
	std::string error;
	REQUIRE(SaveSharedStoreFile(file.Path, BusKind::DataStore, written, error) == SharedStoreFileStatus::Ok);

	std::vector<SharedStoreEntry> read;
	REQUIRE(LoadSharedStoreFile(file.Path, BusKind::DataStore, read, error) == SharedStoreFileStatus::Ok);
	REQUIRE(read.size() == 2);
	CHECK(read[0].Key == Name("a"));
	CHECK(read[0].Version == 2);
	CHECK(read[0].Value == Bytes(std::string_view("a\0b", 3)));
	CHECK(read[1].Key == Name("z"));
}

TEST_CASE("a wrong store or malformed file replaces no caller state", "[world][shared-store-file]") {
	TemporaryFile file("atomic-shared-store-malformed.bin");
	const std::vector<SharedStoreEntry> written{
		{BusKind::MemoryStore, Name("held"), Bytes("value"), 0},
	};
	std::string error;
	REQUIRE(
		SaveSharedStoreFile(file.Path, BusKind::MemoryStore, written, error) == SharedStoreFileStatus::Ok
	);

	std::vector<SharedStoreEntry> read{{BusKind::DataStore, Name("untouched"), Bytes("old"), 1}};
	CHECK(
		LoadSharedStoreFile(file.Path, BusKind::DataStore, read, error) == SharedStoreFileStatus::WrongStore
	);
	CHECK(read[0].Key == Name("untouched"));

	std::ofstream broken(file.Path, std::ios::binary | std::ios::trunc);
	broken.write("bad", 3);
	broken.close();
	CHECK(
		LoadSharedStoreFile(file.Path, BusKind::DataStore, read, error) == SharedStoreFileStatus::Malformed
	);
	CHECK(read[0].Key == Name("untouched"));
}

TEST_CASE("a missing shared store file is an empty environment, not an error", "[world][shared-store-file]") {
	TemporaryFile file("atomic-shared-store-missing.bin");
	std::vector<SharedStoreEntry> read;
	std::string error;
	CHECK(LoadSharedStoreFile(file.Path, BusKind::DataStore, read, error) == SharedStoreFileStatus::NotFound);
	CHECK(read.empty());
	CHECK(error.empty());
}
