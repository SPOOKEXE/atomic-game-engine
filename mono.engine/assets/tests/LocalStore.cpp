// The content folder every program agrees on.
//
// **Every case here builds its store in a temporary directory**, which is what
// `LocalPathsUnder` exists for: a suite that used `DefaultLocalPaths` would write
// into the developer's own `~/Documents`, and a suite that *cleared* it first
// would delete their content.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/LocalStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

TEST_SUITE_ID("engine.assets.localstore")
TEST_DEPENDS("engine.assets.chunkstore")
TEST_DEPENDS("engine.assets.manifest")
TEST_DEPENDS("engine.assets.signature")

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

TEST_CASE("the default paths are the ones the roadmap names", "[assets][localstore]") {
	const engine::assets::LocalPaths paths = engine::assets::DefaultLocalPaths();

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

TEST_CASE("creating a store twice is not an error", "[assets][localstore]") {
	const Scratch scratch("ensure");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	REQUIRE(engine::assets::EnsureLocalStore(paths));
	REQUIRE(std::filesystem::is_directory(paths.Raw));
	REQUIRE(std::filesystem::is_directory(paths.Processed));

	// Idempotent, which is what lets every program call it at startup without
	// asking first.
	REQUIRE(engine::assets::EnsureLocalStore(paths));
}

TEST_CASE("an imported file is named by its own hash", "[assets][localstore]") {
	const Scratch scratch("import");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	const std::filesystem::path source = WriteFile(scratch.Path / "incoming" / "diffuse.png", "pixels");
	const auto report = engine::assets::ImportFile(paths, source, 1000);

	REQUIRE(report.has_value());
	REQUIRE(report->Bytes == 6);
	REQUIRE_FALSE(report->Duplicate);
	REQUIRE(std::filesystem::exists(report->Stored));

	// `<hash><extension>` - the hash is what makes the two folders line up, and
	// the extension is what keeps the folder readable.
	REQUIRE(report->Stored.filename().string() == report->Hash + ".png");
	REQUIRE(report->Hash.size() == 64);

	// **Re-importing is a success and not a second copy.** The bytes are the
	// identity, so the same file arriving twice is already there.
	const auto again = engine::assets::ImportFile(paths, source, 1001);
	REQUIRE(again.has_value());
	REQUIRE(again->Duplicate);
	REQUIRE(again->Stored == report->Stored);

	// **Two files with the same bytes and different names are one file.** That
	// is what content addressing means and it is worth pinning, because the
	// alternative - a suffix - would be a worse hash.
	const std::filesystem::path twin = WriteFile(scratch.Path / "incoming" / "other.png", "pixels");
	const auto copy = engine::assets::ImportFile(paths, twin, 1002);
	REQUIRE(copy.has_value());
	REQUIRE(copy->Duplicate);

	// Different bytes are a different file.
	const std::filesystem::path other = WriteFile(scratch.Path / "incoming" / "normal.png", "different");
	const auto second = engine::assets::ImportFile(paths, other, 1003);
	REQUIRE(second.has_value());
	REQUIRE_FALSE(second->Duplicate);
	REQUIRE(second->Hash != report->Hash);
}

TEST_CASE("the log records where content came from", "[assets][localstore]") {
	const Scratch scratch("log");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	// **A name with a space in it**, which is why the separator is a tab: the
	// roadmap's own seed list is full of them, and a space-separated log would
	// split one of these into two fields.
	const std::filesystem::path source =
		WriteFile(scratch.Path / "incoming" / "Moonlit Slumber.opus", "audio");

	REQUIRE(engine::assets::ImportFile(paths, source, 4242).has_value());

	const std::vector<engine::assets::LogEntry> entries = engine::assets::ReadLog(paths);
	REQUIRE(entries.size() == 1);
	REQUIRE(entries[0].Seconds == 4242);
	REQUIRE(entries[0].Action == "import");
	REQUIRE(entries[0].Bytes == 5);

	// The original path, which the hash-named folder cannot answer and which is
	// the whole reason the log exists.
	REQUIRE(entries[0].Subject == source.string());
	REQUIRE(entries[0].Subject.find("Moonlit Slumber") != std::string::npos);

	// A second import appends rather than rewriting.
	REQUIRE(engine::assets::ImportFile(paths, source, 4243).has_value());
	REQUIRE(engine::assets::ReadLog(paths).size() == 2);
	REQUIRE(engine::assets::ReadLog(paths)[1].Action == "import-duplicate");
}

TEST_CASE("a truncated log line is skipped rather than refusing the file", "[assets][localstore]") {
	const Scratch scratch("truncated");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	engine::assets::LogEntry good;
	good.Seconds = 1;
	good.Action = "import";
	good.Subject = "/art/fox.png";
	good.Hash = "abc";
	good.Bytes = 12;
	REQUIRE(engine::assets::AppendLog(paths, good));

	// What a crash mid-append leaves. **Skipped rather than refused**, because a
	// log several processes append to will have one of these eventually and
	// losing every line before it would be the worse failure.
	{
		std::ofstream file(paths.Log, std::ios::app);
		file << "9999\timport\t/art/half\n";
	}

	REQUIRE(engine::assets::AppendLog(paths, good));

	const std::vector<engine::assets::LogEntry> entries = engine::assets::ReadLog(paths);
	REQUIRE(entries.size() == 2);
	REQUIRE(entries[0].Subject == "/art/fox.png");
	REQUIRE(entries[1].Subject == "/art/fox.png");
}

// --- listing the store, added at v0.10 ----------------------------------------
//
// **What the assets panel and the mesh picker both read.** The panel used to
// list the *log*, whose subjects are the paths files came from - so most of what
// it showed was somewhere else on the disk and some of it no longer existed.
// These two are the store describing itself.

TEST_CASE("the raw listing is the folder, labelled by the log", "[assets][localstore]") {
	const Scratch scratch("rawlist");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	const std::filesystem::path first = WriteFile(scratch.Path / "art" / "Fox Diffuse.png", "pixels");
	REQUIRE(engine::assets::ImportFile(paths, first, 100).has_value());

	const std::filesystem::path second = WriteFile(scratch.Path / "audio" / "hit.wav", "samples");
	REQUIRE(engine::assets::ImportFile(paths, second, 200).has_value());

	const std::vector<engine::assets::RawEntry> raw = engine::assets::RawContents(paths);
	REQUIRE(raw.size() == 2);

	// **The original names, which only the log can supply**: `raw/` is
	// hash-named, so a listing without the log would be two rows of hex.
	std::vector<std::string> names;
	for (const engine::assets::RawEntry &entry : raw) {
		names.push_back(entry.Original);
		CHECK(entry.Path.parent_path() == paths.Raw);
		CHECK(entry.Bytes > 0);
	}
	std::sort(names.begin(), names.end());
	CHECK(names[0] == "Fox Diffuse.png");
	CHECK(names[1] == "hit.wav");
}

TEST_CASE("a file dropped into raw by hand is still listed", "[assets][localstore]") {
	// **The log labels and never enumerates**, which is the whole distinction:
	// a listing built *from* the log would miss this file entirely, and would
	// show rows for files somebody had since deleted.
	const Scratch scratch("rawhand");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	WriteFile(paths.Raw / "deadbeef.png", "pixels");

	const std::vector<engine::assets::RawEntry> raw = engine::assets::RawContents(paths);
	REQUIRE(raw.size() == 1);

	// With no log line, the file name is the best label there is - and it is
	// the honest one rather than a blank.
	CHECK(raw[0].Original == "deadbeef.png");
}

TEST_CASE("a tree under raw is listed and labelled by its path", "[assets][localstore]") {
	// **`cdn::Publish` walks `raw/` recursively and names by relative path**, so
	// a tree there has always been publishable - and v0.10's material import is
	// the first thing that writes one, because a material has to *name* its
	// texture and `ImportFile`'s hash rename gives it no name to write. The
	// listing was not recursive, so a store full of materials read as empty.
	const Scratch scratch("rawtree");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	WriteFile(paths.Raw / "materials" / "ambientcg" / "Bricks075A.amat", "material");
	WriteFile(paths.Raw / "materials" / "ambientcg" / "Bricks075A_Color.atex", "pixels");

	const std::vector<engine::assets::RawEntry> raw = engine::assets::RawContents(paths);
	REQUIRE(raw.size() == 2);

	// **The path relative to `raw/`, not the file name**, because that is what
	// the publisher will call it - and two materials in different source folders
	// routinely share a leaf.
	std::vector<std::string> names;
	for (const engine::assets::RawEntry &entry : raw) {
		names.push_back(entry.Original);
	}
	std::sort(names.begin(), names.end());
	CHECK(names[0] == "materials/ambientcg/Bricks075A.amat");
	CHECK(names[1] == "materials/ambientcg/Bricks075A_Color.atex");
}

TEST_CASE("an asset is found in baked before raw", "[assets][localstore]") {
	// **The regression this exists for was silent and looked like missing
	// content.** Every preview in the editor read `raw/<name>`, with a comment
	// saying that held because the publisher walked `raw/`. The day it walked
	// `baked/`, every published name resolved to a file that was not there and
	// each one quietly became "no local pixels" - a store full of assets showing
	// a grid of empty boxes, with nothing logged and nothing to grep for.
	const Scratch scratch("find");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	WriteFile(paths.Raw / "sheet.png", "source");
	WriteFile(paths.Baked / "sheet.atex", "baked");

	// A published name is a path under `baked/`, which is what the publisher
	// walked.
	CHECK(engine::assets::FindInStore(paths, "sheet.atex") == paths.Baked / "sheet.atex");

	// A raw listing's name is a path under `raw/`, and both reach the same
	// caller - a picker showing either half.
	CHECK(engine::assets::FindInStore(paths, "sheet.png") == paths.Raw / "sheet.png");

	// **`baked/` wins when both have the name**, because that is the one a
	// manifest can be naming: a stale source beside a fresh bake would otherwise
	// preview the thing nobody publishes.
	WriteFile(paths.Raw / "both.atex", "stale");
	WriteFile(paths.Baked / "both.atex", "fresh");
	CHECK(engine::assets::FindInStore(paths, "both.atex") == paths.Baked / "both.atex");

	// Nothing anywhere is an empty path rather than a plausible one - the honest
	// answer for an asset published from another machine.
	CHECK(engine::assets::FindInStore(paths, "absent.atex").empty());
	CHECK(engine::assets::FindInStore(paths, "").empty());

	// A tree, because the material import writes one and a picker shows it.
	WriteFile(paths.Baked / "materials" / "oak.amat", "material");
	CHECK(engine::assets::FindInStore(paths, "materials/oak.amat") == paths.Baked / "materials" / "oak.amat");
}

TEST_CASE("the development identity is one constant everything agrees on", "[assets][localstore]") {
	// **Stable across calls and across processes**, which is the whole of what it
	// buys: `contentimport --publish` signs with it and a client with no
	// `--publisher-key` trusts it, and neither has to be told the other's half.
	CHECK(
		engine::assets::DevelopmentSigningKey().Public().ToHex() ==
		engine::assets::DevelopmentPublisher().ToHex()
	);
	CHECK(engine::assets::DevelopmentPublisher().ToHex() == engine::assets::DevelopmentPublisher().ToHex());

	// Not zero, which is what a key that failed to build would read as.
	CHECK(engine::assets::DevelopmentPublisher().ToHex().size() == 64);
	CHECK(engine::assets::DevelopmentPublisher().ToHex() != std::string(64, '0'));
}

TEST_CASE("a store with nothing in it lists nothing rather than failing", "[assets][localstore]") {
	const Scratch scratch("rawempty");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	// Never created at all: the panel calls this before anything exists.
	CHECK(engine::assets::RawContents(paths).empty());
	CHECK(engine::assets::PublishedContents(paths).empty());

	REQUIRE(engine::assets::EnsureLocalStore(paths));
	CHECK(engine::assets::RawContents(paths).empty());
	CHECK(engine::assets::PublishedContents(paths).empty());
}

TEST_CASE("the published listing gives the names and kinds a scene writes", "[assets][localstore]") {
	const Scratch scratch("published");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	auto store = engine::assets::ChunkStore::Open(paths.Processed, true);
	REQUIRE(store.has_value());

	engine::assets::Manifest manifest;
	manifest.AddAsset("rock.mesh", engine::assets::AssetKind::Mesh, {});
	manifest.AddAsset("grass.atex", engine::assets::AssetKind::Texture, {});
	manifest.AddAsset("step.wav", engine::assets::AssetKind::Audio, {});
	REQUIRE(store->WriteManifest(manifest, {}));

	const std::vector<engine::assets::PublishedEntry> published = engine::assets::PublishedContents(paths);
	REQUIRE(published.size() == 3);

	// **Name order**, which is what a picker wants - and sorted here rather
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
}

TEST_CASE("a model's texture is followed through the import log", "[assets][localstore]") {
	// **The link importing destroys, put back.** A `.pmx` names its sheets
	// relative to the folder it was authored in - `tex/skin.png` - and
	// `ImportFile` renames every file to `<hash><extension>` in one flat
	// directory. After that the folder cannot say that a particular `<hash>.png`
	// is the model's `tex/skin.png`, so a bake joins the reference lexically,
	// writes `tex/skin.atex` into the mesh, and publishes a model that arrives,
	// draws, and is untextured with nothing anywhere saying why.
	//
	// The log still records where each file came from, and the two original
	// paths share a directory exactly as the model expects.
	const Scratch scratch("resolver");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	// An art folder as somebody would have it: the model, and its sheets in a
	// `tex/` folder beside it.
	const std::filesystem::path art = scratch.Path / "art" / "character";
	const std::filesystem::path model = WriteFile(art / "hero.pmx", "model bytes");
	const std::filesystem::path skin = WriteFile(art / "tex" / "skin.png", "skin pixels");
	const std::filesystem::path hair = WriteFile(art / "tex" / "hair.png", "hair pixels");

	const auto modelReport = engine::assets::ImportFile(paths, model, 1000);
	const auto skinReport = engine::assets::ImportFile(paths, skin, 1001);
	const auto hairReport = engine::assets::ImportFile(paths, hair, 1002);
	REQUIRE(modelReport.has_value());
	REQUIRE(skinReport.has_value());
	REQUIRE(hairReport.has_value());

	const auto resolve = engine::assets::StoreTextureResolver(paths);
	const std::string stored = modelReport->Stored.filename().string();

	// **The extension comes from the original**, so the answer names a file that
	// is actually in `raw/` - `BakedName` is applied by the caller and turns this
	// into the `.atex` the manifest carries.
	std::string out;
	REQUIRE(resolve(stored, "tex/skin.png", out));
	CHECK(out == skinReport->Hash + ".png");
	CHECK(std::filesystem::is_regular_file(paths.Raw / out));

	REQUIRE(resolve(stored, "tex/hair.png", out));
	CHECK(out == hairReport->Hash + ".png");

	// **A reference up and out of the model's own folder is ordinary** - shared
	// sheets live one level up all the time - so the join is normalised rather
	// than taken literally. `character/tex/../tex/skin.png` is `tex/skin.png`.
	REQUIRE(resolve(stored, "tex/../tex/skin.png", out));
	CHECK(out == skinReport->Hash + ".png");
}

TEST_CASE("an unresolvable texture is refused rather than guessed", "[assets][localstore]") {
	// **Every `false` here becomes a cleared reference and a warning in the
	// bake**, which is the whole correction: the old path emitted a well-formed
	// name for an asset that would never exist, and nothing downstream could tell
	// that apart from content still streaming in.
	const Scratch scratch("resolver_miss");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);
	REQUIRE(engine::assets::EnsureLocalStore(paths));

	const std::filesystem::path art = scratch.Path / "art";
	const auto imported = engine::assets::ImportFile(paths, WriteFile(art / "hero.pmx", "model bytes"), 1000);
	REQUIRE(imported.has_value());

	const auto resolve = engine::assets::StoreTextureResolver(paths);
	std::string out;

	// The sheet was never imported, so nothing can place it.
	CHECK_FALSE(resolve(imported->Stored.filename().string(), "tex/skin.png", out));

	// And a model that is not in the log at all - dropped into `raw/` by hand -
	// cannot be placed either. That is a `false` rather than a guess: the folder
	// is the index, and the log only ever labels.
	CHECK_FALSE(resolve("deadbeef.pmx", "tex/skin.png", out));
}

TEST_CASE("an empty file is refused rather than stored", "[assets][localstore]") {
	const Scratch scratch("empty");
	const engine::assets::LocalPaths paths = engine::assets::LocalPathsUnder(scratch.Path);

	const std::filesystem::path source = WriteFile(scratch.Path / "incoming" / ".lock", "");

	// **The whole of D00034 in one case.** An empty file hashes to BLAKE3's
	// empty-input digest, writes a zero-byte `raw/` entry, and bakes to nothing
	// - so the store's raw and baked counts differ by one for ever with no line
	// anywhere naming it. This repository's own store carried exactly that for a
	// version: a `.lock` from inside a Python virtualenv, swept along by a
	// folder import of a model.
	CHECK_FALSE(engine::assets::ImportFile(paths, source, 1000).has_value());

	// **Nothing is left behind**, which is the half that matters. A refusal that
	// still wrote the file would move the silence rather than remove it.
	size_t stored = 0;
	std::error_code failure;
	for (const auto &entry : std::filesystem::directory_iterator(paths.Raw, failure)) {
		if (entry.is_regular_file()) {
			stored++;
		}
	}
	CHECK(stored == 0);

	// And it is not in the log either, so "where did this come from" cannot
	// answer for a file that was never taken in.
	const std::vector<engine::assets::LogEntry> log = engine::assets::ReadLog(paths);
	CHECK(log.empty());
}
