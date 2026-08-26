#include <engine/assets/LocalStore.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/LocalPublish.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("cdn.localpublish")
TEST_DEPENDS("engine.assets.localstore")
TEST_DEPENDS("cdn.publisher")

namespace {
	struct Scratch {
		std::filesystem::path Path;

		explicit Scratch(const char *name)
			: Path(std::filesystem::temp_directory_path() / ("atomic-localpublish-" + std::string(name))) {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}

		~Scratch() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}

		Scratch(const Scratch &) = delete;
		Scratch &operator=(const Scratch &) = delete;
	};

	std::filesystem::path WriteFile(const std::filesystem::path &where, std::string_view text) {
		std::filesystem::create_directories(where.parent_path());
		std::ofstream file(where, std::ios::binary);
		file << text;
		return where;
	}

	engine::assets::SigningKey SigningKey() {
		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		seed.fill(std::byte{7});
		auto signing = engine::assets::SigningKey::FromSeed(seed);
		REQUIRE(signing.has_value());
		return std::move(*signing);
	}
}

TEST_CASE("publishing the baked folder fills the processed one", "[cdn][localpublish]") {
	const Scratch scratch("publish");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	REQUIRE(engine::assets::ImportFile(paths, WriteFile(scratch.Path / "in" / "a.txt", "alpha"), 1));
	REQUIRE(engine::assets::ImportFile(paths, WriteFile(scratch.Path / "in" / "b.txt", "beta"), 2));
	WriteFile(paths.Baked / "a.txt", "alpha");
	WriteFile(paths.Baked / "b.txt", "beta");

	const auto report = cdn::PublishLocal(paths, SigningKey(), 3);
	REQUIRE(report.has_value());
	CHECK(report->Assets == 2);
	CHECK_FALSE(report->Root.IsZero());
	CHECK_FALSE(std::filesystem::is_empty(paths.Processed));

	const std::vector<engine::assets::LogEntry> entries = engine::assets::ReadLog(paths);
	REQUIRE(entries.size() == 3);
	CHECK(entries[2].Action == "publish");
	CHECK(entries[2].Hash == report->Root.ToHex());
}

TEST_CASE("publishing an unbaked store is refused", "[cdn][localpublish]") {
	const Scratch scratch("unbaked");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::ImportFile(paths, WriteFile(scratch.Path / "in" / "a.png", "pixels"), 1));

	CHECK_FALSE(cdn::PublishLocal(paths, SigningKey(), 2));
}
