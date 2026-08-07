// The content folder every program agrees on.
//
// **Every case here builds its store in a temporary directory**, which is what
// `LocalPathsUnder` exists for: a suite that used `DefaultLocalPaths` would write
// into the developer's own `~/Documents`, and a suite that *cleared* it first
// would delete their content.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// --- listing the store, added at v0.10 ----------------------------------------
//
// **What the assets panel and the mesh picker both read.** The panel used to
// list the *log*, whose subjects are the paths files came from — so most of what
// it showed was somewhere else on the disk and some of it no longer existed.
// These two are the store describing itself.

TEST_CASE("the raw listing is the folder, labelled by the log", "[cdn]") {
	const Scratch scratch("rawlist");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);

	const std::filesystem::path first = WriteFile(scratch.Path / "art" / "Fox Diffuse.png", "pixels");
	REQUIRE(cdn::ImportFile(paths, first, 100).has_value());

	const std::filesystem::path second = WriteFile(scratch.Path / "audio" / "hit.wav", "samples");
	REQUIRE(cdn::ImportFile(paths, second, 200).has_value());

	const std::vector<cdn::RawEntry> raw = cdn::RawContents(paths);
	REQUIRE(raw.size() == 2);

	// **The original names, which only the log can supply**: `raw/` is
	// hash-named, so a listing without the log would be two rows of hex.
	std::vector<std::string> names;
	for (const cdn::RawEntry &entry : raw) {
		names.push_back(entry.Original);
		CHECK(entry.Path.parent_path() == paths.Raw);
		CHECK(entry.Bytes > 0);
	}
	std::sort(names.begin(), names.end());
	CHECK(names[0] == "Fox Diffuse.png");
	CHECK(names[1] == "hit.wav");
}

TEST_CASE("a file dropped into raw by hand is still listed", "[cdn]") {
	// **The log labels and never enumerates**, which is the whole distinction:
	// a listing built *from* the log would miss this file entirely, and would
	// show rows for files somebody had since deleted.
	const Scratch scratch("rawhand");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);
	REQUIRE(cdn::EnsureLocalStore(paths));

	WriteFile(paths.Raw / "deadbeef.png", "pixels");

	const std::vector<cdn::RawEntry> raw = cdn::RawContents(paths);
	REQUIRE(raw.size() == 1);

	// With no log line, the file name is the best label there is — and it is
	// the honest one rather than a blank.
	CHECK(raw[0].Original == "deadbeef.png");
}

TEST_CASE("a tree under raw is listed and labelled by its path", "[cdn]") {
	// **`cdn::Publish` walks `raw/` recursively and names by relative path**, so
	// a tree there has always been publishable — and v0.10's material import is
	// the first thing that writes one, because a material has to *name* its
	// texture and `ImportFile`'s hash rename gives it no name to write. The
	// listing was not recursive, so a store full of materials read as empty.
	const Scratch scratch("rawtree");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);
	REQUIRE(cdn::EnsureLocalStore(paths));

	WriteFile(paths.Raw / "materials" / "ambientcg" / "Bricks075A.amat", "material");
	WriteFile(paths.Raw / "materials" / "ambientcg" / "Bricks075A_Color.atex", "pixels");

	const std::vector<cdn::RawEntry> raw = cdn::RawContents(paths);
	REQUIRE(raw.size() == 2);

	// **The path relative to `raw/`, not the file name**, because that is what
	// the publisher will call it — and two materials in different source folders
	// routinely share a leaf.
	std::vector<std::string> names;
	for (const cdn::RawEntry &entry : raw) {
		names.push_back(entry.Original);
	}
	std::sort(names.begin(), names.end());
	CHECK(names[0] == "materials/ambientcg/Bricks075A.amat");
	CHECK(names[1] == "materials/ambientcg/Bricks075A_Color.atex");
}

TEST_CASE("a store with nothing in it lists nothing rather than failing", "[cdn]") {
	const Scratch scratch("rawempty");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);

	// Never created at all: the panel calls this before anything exists.
	CHECK(cdn::RawContents(paths).empty());
	CHECK(cdn::PublishedContents(paths).empty());

	REQUIRE(cdn::EnsureLocalStore(paths));
	CHECK(cdn::RawContents(paths).empty());
	CHECK(cdn::PublishedContents(paths).empty());
}

TEST_CASE("the published listing gives the names and kinds a scene writes", "[cdn]") {
	const Scratch scratch("published");
	const cdn::LocalPaths paths = cdn::LocalPathsUnder(scratch.Path);
	REQUIRE(cdn::EnsureLocalStore(paths));

	// Written straight into `raw/` under names a publisher can classify, which
	// is what `ImportFile` produces once `assetc` has baked a tree.
	WriteFile(paths.Raw / "rock.mesh", "vertices");
	WriteFile(paths.Raw / "grass.atex", "texels");
	WriteFile(paths.Raw / "step.wav", "samples");

	std::array<std::byte, 32> seed{};
	seed.fill(std::byte{9});
	const auto signing = engine::assets::SigningKey::FromSeed(seed);
	REQUIRE(signing.has_value());
	REQUIRE(cdn::PublishLocal(paths, *signing, 1).has_value());

	const std::vector<cdn::PublishedEntry> published = cdn::PublishedContents(paths);
	REQUIRE(published.size() == 3);

	// **Name order**, which is what a picker wants — and sorted here rather
	// than relied upon, because the manifest happening to be sorted is a
	// property of the publisher and not of the format.
	CHECK(published[0].Name == "grass.atex");
	CHECK(published[1].Name == "rock.mesh");
	CHECK(published[2].Name == "step.wav");

	// The kinds are what makes a picker able to offer meshes to a `Mesh`
	// property and textures to a `Texture` one.
	CHECK(published[0].Kind == engine::assets::AssetKind::Texture);
	CHECK(published[1].Kind == engine::assets::AssetKind::Mesh);
	CHECK(published[2].Kind == engine::assets::AssetKind::Audio);

	for (const cdn::PublishedEntry &entry : published) {
		CHECK_FALSE(entry.Root.IsZero());
	}
}
