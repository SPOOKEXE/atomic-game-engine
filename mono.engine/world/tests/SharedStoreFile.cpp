#include <engine/testing/Suite.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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

TEST_CASE("shared store images commit canonical bytes only after validation", "[world][shared-store-file]") {
	using engine::world::DecodeSharedStoreImage;
	using engine::world::EncodeSharedStoreImage;
	std::vector<SharedStoreEntry> entries{
		{BusKind::DataStore, Name("z"), Bytes("last"), 7},
		{BusKind::DataStore, Name("a"), Bytes(std::string_view("a\0b", 3)), 2},
	};
	std::vector<std::byte> encoded = Bytes("previous longer buffer contents");
	std::string error;
	REQUIRE(EncodeSharedStoreImage(BusKind::DataStore, entries, encoded, error) == SharedStoreFileStatus::Ok);
	CHECK(error.empty());
	// Fixed format-one bytes include lexical key order, versions and a binary value.
	std::string hex;
	constexpr std::string_view digits = "0123456789abcdef";
	for (const auto byte : encoded) {
		const auto value = std::to_integer<unsigned>(byte);
		hex += digits[value >> 4];
		hex += digits[value & 15];
	}
	CHECK(
		hex == "41445353544f52450100000002020000000100000061020000000000000003000000610062010000007a070000000"
			   "0000000040000006c617374"
	);
	const auto canonical = encoded;
	std::reverse(entries.begin(), entries.end());
	REQUIRE(EncodeSharedStoreImage(BusKind::DataStore, entries, encoded, error) == SharedStoreFileStatus::Ok);
	CHECK(encoded == canonical);
	std::vector<SharedStoreEntry> decoded;
	REQUIRE(DecodeSharedStoreImage(encoded, BusKind::DataStore, decoded, error) == SharedStoreFileStatus::Ok);
	CHECK(decoded == entries);

	SECTION("duplicate keys and invalid versions preserve the previous output") {
		entries[1].Key = entries[0].Key;
		CHECK(
			EncodeSharedStoreImage(BusKind::DataStore, entries, encoded, error) ==
			SharedStoreFileStatus::Malformed
		);
		CHECK(encoded == canonical);
		CHECK_FALSE(error.empty());
		entries.resize(1);
		entries[0].Version = 0;
		CHECK(
			EncodeSharedStoreImage(BusKind::DataStore, entries, encoded, error) ==
			SharedStoreFileStatus::Malformed
		);
		CHECK(encoded == canonical);
		CHECK(
			EncodeSharedStoreImage(BusKind::Messaging, entries, encoded, error) ==
			SharedStoreFileStatus::WrongStore
		);
		CHECK(encoded == canonical);
	}

	SECTION("the output can replace a source value after all source bytes are consumed") {
		const auto original = entries;
		REQUIRE(
			EncodeSharedStoreImage(BusKind::DataStore, entries, entries[0].Value, error) ==
			SharedStoreFileStatus::Ok
		);
		CHECK(entries[0].Value == canonical);
		REQUIRE(
			DecodeSharedStoreImage(entries[0].Value, BusKind::DataStore, decoded, error) ==
			SharedStoreFileStatus::Ok
		);
		CHECK(decoded == original);
	}
}
