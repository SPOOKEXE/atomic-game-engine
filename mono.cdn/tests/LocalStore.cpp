// The content folder every program agrees on.
//
// **Every case here builds its store in a temporary directory**, which is what
// `LocalPathsUnder` exists for: a suite that used `DefaultLocalPaths` would write
// into the developer's own `~/Documents`, and a suite that *cleared* it first
// would delete their content.

#include <engine/assets/Signature.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/LocalStore.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

TEST_SUITE_ID("cdn.localstore")

namespace {
	// A directory that goes away with the test.
	struct Scratch {
		std::filesystem::path Path;

		explicit Scratch(const char *name)
			: Path(std::filesystem::temp_directory_path() / ("atomic-localstore-" + std::string(name))) {
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
}

TEST_CASE("the default paths are the ones the roadmap names", "[cdn]") {
	const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

	// **The shape and not the prefix**, because the prefix is somebody's home
	// directory and differs on every machine. What is pinned is the part
	// `ROADMAP.md` actually specifies.
	REQUIRE(paths.Root.parent_path().filename() == "atomic-game-engine");
	REQUIRE(paths.Root.filename() == "cdn");
	REQUIRE(paths.Raw == paths.Root / "raw");
	REQUIRE(paths.Processed == paths.Root / "processed");

	// The log is in the store's root rather than in either folder, because it
	// records what happened to both.
	REQUIRE(paths.Log.parent_path() == paths.Root);
}

TEST_CASE("creating a store twice is not an error", "[cdn]") {
	const Scratch scratch("ensure");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);

	REQUIRE(cdn::EnsureLocalStore(paths));
	REQUIRE(std::filesystem::is_directory(paths.Raw));
	REQUIRE(std::filesystem::is_directory(paths.Processed));

	// Idempotent, which is what lets every program call it at startup without
	// asking first.
	REQUIRE(cdn::EnsureLocalStore(paths));
}

TEST_CASE("an imported file is named by its own hash", "[cdn]") {
	const Scratch scratch("import");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);

	const std::filesystem::path source = WriteFile(scratch.Path / "incoming" / "diffuse.png", "pixels");
	const auto report = cdn::ImportFile(paths, source, 1000);

	REQUIRE(report.has_value());
	REQUIRE(report->Bytes == 6);
	REQUIRE_FALSE(report->Duplicate);
	REQUIRE(std::filesystem::exists(report->Stored));

	// `<hash><extension>` — the hash is what makes the two folders line up, and
	// the extension is what keeps the folder readable.
	REQUIRE(report->Stored.filename().string() == report->Hash + ".png");
	REQUIRE(report->Hash.size() == 64);

	// **Re-importing is a success and not a second copy.** The bytes are the
	// identity, so the same file arriving twice is already there.
	const auto again = cdn::ImportFile(paths, source, 1001);
	REQUIRE(again.has_value());
	REQUIRE(again->Duplicate);
	REQUIRE(again->Stored == report->Stored);

	// **Two files with the same bytes and different names are one file.** That
	// is what content addressing means and it is worth pinning, because the
	// alternative — a suffix — would be a worse hash.
	const std::filesystem::path twin = WriteFile(scratch.Path / "incoming" / "other.png", "pixels");
	const auto copy = cdn::ImportFile(paths, twin, 1002);
	REQUIRE(copy.has_value());
	REQUIRE(copy->Duplicate);

	// Different bytes are a different file.
	const std::filesystem::path other = WriteFile(scratch.Path / "incoming" / "normal.png", "different");
	const auto second = cdn::ImportFile(paths, other, 1003);
	REQUIRE(second.has_value());
	REQUIRE_FALSE(second->Duplicate);
	REQUIRE(second->Hash != report->Hash);
}

TEST_CASE("the log records where content came from", "[cdn]") {
	const Scratch scratch("log");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);

	// **A name with a space in it**, which is why the separator is a tab: the
	// roadmap's own seed list is full of them, and a space-separated log would
	// split one of these into two fields.
	const std::filesystem::path source =
		WriteFile(scratch.Path / "incoming" / "Moonlit Slumber.opus", "audio");

	REQUIRE(cdn::ImportFile(paths, source, 4242).has_value());

	const std::vector<cdn::LogEntry> entries = cdn::ReadLog(paths);
	REQUIRE(entries.size() == 1);
	REQUIRE(entries[0].Seconds == 4242);
	REQUIRE(entries[0].Action == "import");
	REQUIRE(entries[0].Bytes == 5);

	// The original path, which the hash-named folder cannot answer and which is
	// the whole reason the log exists.
	REQUIRE(entries[0].Subject == source.string());
	REQUIRE(entries[0].Subject.find("Moonlit Slumber") != std::string::npos);

	// A second import appends rather than rewriting.
	REQUIRE(cdn::ImportFile(paths, source, 4243).has_value());
	REQUIRE(cdn::ReadLog(paths).size() == 2);
	REQUIRE(cdn::ReadLog(paths)[1].Action == "import-duplicate");
}

TEST_CASE("a truncated log line is skipped rather than refusing the file", "[cdn]") {
	const Scratch scratch("truncated");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);
	REQUIRE(cdn::EnsureLocalStore(paths));

	cdn::LogEntry good;
	good.Seconds = 1;
	good.Action = "import";
	good.Subject = "/art/fox.png";
	good.Hash = "abc";
	good.Bytes = 12;
	REQUIRE(cdn::AppendLog(paths, good));

	// What a crash mid-append leaves. **Skipped rather than refused**, because a
	// log several processes append to will have one of these eventually and
	// losing every line before it would be the worse failure.
	{
		std::ofstream file(paths.Log, std::ios::app);
		file << "9999\timport\t/art/half\n";
	}

	REQUIRE(cdn::AppendLog(paths, good));

	const std::vector<cdn::LogEntry> entries = cdn::ReadLog(paths);
	REQUIRE(entries.size() == 2);
	REQUIRE(entries[0].Subject == "/art/fox.png");
	REQUIRE(entries[1].Subject == "/art/fox.png");
}

TEST_CASE("publishing the raw folder fills the processed one", "[cdn]") {
	const Scratch scratch("publish");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);

	REQUIRE(cdn::ImportFile(paths, WriteFile(scratch.Path / "in" / "a.txt", "alpha"), 1).has_value());
	REQUIRE(cdn::ImportFile(paths, WriteFile(scratch.Path / "in" / "b.txt", "beta"), 2).has_value());

	// **The key is the caller's**, because a store on disk has no business
	// holding one — a key beside the content it signs is a key that signs
	// anything anybody drops there.
	std::array<std::byte, 32> seed{};
	seed.fill(std::byte{7});
	const auto signing = engine::assets::SigningKey::FromSeed(seed);
	REQUIRE(signing.has_value());

	const auto report = cdn::PublishLocal(paths, *signing, 3);
	REQUIRE(report.has_value());
	REQUIRE(report->Assets == 2);
	REQUIRE_FALSE(report->Root.IsZero());
	REQUIRE_FALSE(std::filesystem::is_empty(paths.Processed));

	// The publish is logged beside the imports, with the manifest root as its
	// hash — which is what makes "which publish is this store" answerable.
	const std::vector<cdn::LogEntry> entries = cdn::ReadLog(paths);
	REQUIRE(entries.size() == 3);
	REQUIRE(entries[2].Action == "publish");
	REQUIRE(entries[2].Hash == report->Root.ToHex());
}
