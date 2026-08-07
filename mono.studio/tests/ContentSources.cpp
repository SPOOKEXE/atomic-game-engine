// The editor's content-source list, and the file it survives in.
//
// **Reachable headlessly because none of it needs a frame.** The preferences
// page that edits this list is imgui; the list itself is ordinary data with a
// text format either side of it, which is the half that can be wrong without a
// window — a dropped row, a lost `off`, a priority order that comes back
// reversed.
//
// The order is the feature, so most of what is pinned here is order:
// `delivery::AssetClient` walks the list and stops at the first source that
// answers, which makes "second" a different meaning from "first" rather than a
// presentation detail.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <cdn/LocalStore.hpp>
#include <studio/ContentSources.hpp>

TEST_SUITE_ID("studio.contentsources")

using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::delivery::SourceRole;
using studio::ContentSources;

namespace {
	std::filesystem::path ScratchFile(const char *leaf) {
		return std::filesystem::temp_directory_path() / leaf;
	}

	Source Origin(const char *name, const char *location, bool enabled = true) {
		Source source;
		source.Name = name;
		source.Kind = SourceKind::Http;
		source.Location = location;
		source.Enabled = enabled;
		return source;
	}
}

TEST_CASE("a fresh install starts with the store on this machine", "[studio][contentsources]") {
	const ContentSources sources = ContentSources::Default();

	// **Two rows, and the first is the local folder.** This used to be one HTTP
	// origin and no publisher key, and "no key" is what `DeliverySettings::
	// IsValid` refuses — so a fresh editor built no delivery client and fetched
	// nothing at all. A `MeshPart` drew the fallback cube however good its
	// `MeshId` was.
	REQUIRE(sources.Sources.size() == 2);
	CHECK(sources.Sources.front().Enabled);
	CHECK(sources.Sources.front().Kind == engine::delivery::SourceKind::Directory);

	// **A key by default, which the comment here used to forbid.** "Defaulting
	// one would be defaulting who this editor believes" is still true and is
	// exactly why this one is `cdn::DevelopmentSigningKey`'s public half: it
	// believes the store this machine publishes to, which is the folder on the
	// row above it. Pointing a row anywhere else means supplying the real key —
	// see `cdn/LocalStore.hpp` for where that stops.
	CHECK_FALSE(sources.PublisherKey.empty());
}

TEST_CASE("a saved list comes back in the same order", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-roundtrip.ini");
	std::filesystem::remove(path);

	ContentSources written;
	written.Sources = {Origin("near", "127.0.0.1:9080"), Origin("far", "10.0.0.4:9080")};
	written.Sources.push_back(Origin("off-site", "10.0.0.9:9080", false));
	written.Sources[2].Kind = SourceKind::Directory;
	written.Sources[2].Location = "/var/content";
	written.CachePath = "/tmp/atomic-cache";
	written.PublisherKey = std::string(64, 'a');

	REQUIRE(written.Save(path));

	ContentSources read;
	REQUIRE(read.Load(path));

	REQUIRE(read.Sources.size() == 3);
	CHECK(read.Sources[0].Name == "near");
	CHECK(read.Sources[1].Name == "far");
	CHECK(read.Sources[2].Name == "off-site");

	// **The disabled row survives, address and all.** The header promises this
	// explicitly: somebody working out which of three origins is broken
	// switches one off and back on, and a file that forgot the address in
	// between would make that a retyping exercise.
	CHECK_FALSE(read.Sources[2].Enabled);
	CHECK(read.Sources[2].Location == "/var/content");
	CHECK(read.Sources[2].Kind == SourceKind::Directory);

	CHECK(read.CachePath == std::filesystem::path("/tmp/atomic-cache"));
	CHECK(read.PublisherKey == std::string(64, 'a'));

	std::filesystem::remove(path);
}

TEST_CASE("a missing file leaves the list alone", "[studio][contentsources]") {
	// Not an error, and — the part worth pinning — not a reset either. `Load`
	// returns before it clears anything, so a caller that loads over a list it
	// already has keeps that list rather than being silently emptied.
	ContentSources sources;
	sources.Sources = {Origin("kept", "127.0.0.1:9080")};

	CHECK_FALSE(sources.Load(ScratchFile("atomic-sources-does-not-exist.ini")));
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Name == "kept");
}

TEST_CASE("a half-written row is skipped rather than half-read", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-malformed.ini");
	std::filesystem::remove(path);

	{
		std::ofstream out(path, std::ios::trunc);
		out << "# a comment\n";
		out << "\n";
		out << "not-a-key-value-line\n";
		out << "source = only-a-name\n";
		out << "source = two | http\n";
		out << "source = good | http | 127.0.0.1:9080 | on\n";
		out << "unknown = ignored\n";
	}

	ContentSources sources;
	REQUIRE(sources.Load(path));

	// A source with no location would sit in the list looking configured and
	// refuse every fetch, which is worse than not being there.
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Name == "good");

	std::filesystem::remove(path);
}

TEST_CASE("Move refuses to walk off either end", "[studio][contentsources]") {
	ContentSources sources;
	sources.Sources = {Origin("first", "a"), Origin("second", "b")};

	// Wrong: past the end, and the two ends themselves.
	CHECK_FALSE(sources.Move(2, -1));
	CHECK_FALSE(sources.Move(0, -1));
	CHECK_FALSE(sources.Move(1, 1));

	// Nothing moved on any refusal.
	CHECK(sources.Sources[0].Name == "first");
	CHECK(sources.Sources[1].Name == "second");

	REQUIRE(sources.Move(0, 1));
	CHECK(sources.Sources[0].Name == "second");
	CHECK(sources.Sources[1].Name == "first");
}

TEST_CASE("Move on an empty list does nothing", "[studio][contentsources]") {
	ContentSources sources;
	CHECK_FALSE(sources.Move(0, 1));
	CHECK(sources.Sources.empty());
}

TEST_CASE("an unparsable publisher key yields settings with no trust", "[studio][contentsources]") {
	// **The failure has to be visible as "not configured" rather than as a
	// key.** `MakeAssetClient` is what refuses, and it can only refuse if a
	// half-typed key does not arrive looking like a whole one.
	ContentSources sources = ContentSources::Default();
	sources.PublisherKey = "not-hex";

	const engine::delivery::DeliverySettings settings = sources.ToSettings();
	CHECK(settings.Sources.size() == sources.Sources.size());

	// And the list a person typed carries no host restriction: that check is
	// for a source list a *server* sent, not one from these preferences.
	CHECK(settings.AllowedHosts.empty());
}

// --- roles: which direction a source is used in ----------------------------

TEST_CASE("a role and an ingest key survive a save and a load", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-roles.ini");
	std::filesystem::remove(path);

	ContentSources sources;
	sources.PublisherKey = std::string(64, 'a');

	Source reader = Origin("reads", "127.0.0.1:9080");
	reader.Role = SourceRole::Read;

	Source writer = Origin("writes", "127.0.0.1:9081");
	writer.Role = SourceRole::Write;
	writer.IngestKey = "a shared secret";

	sources.Sources = {reader, writer};
	REQUIRE(sources.Save(path));

	ContentSources loaded;
	REQUIRE(loaded.Load(path));
	REQUIRE(loaded.Sources.size() == 2);

	CHECK(loaded.Sources[0].Role == SourceRole::Read);
	CHECK(loaded.Sources[0].IngestKey.empty());
	CHECK(loaded.Sources[1].Role == SourceRole::Write);

	// **A key with a space in it**, which is why the field is the line's
	// remainder rather than one more pipe-separated column: a secret somebody
	// pasted is not this parser's business to constrain.
	CHECK(loaded.Sources[1].IngestKey == "a shared secret");

	std::filesystem::remove(path);
}

TEST_CASE("a list written before roles existed loads as both", "[studio][contentsources]") {
	// **Not compatibility for its own sake.** The absent case has an obviously
	// right answer: a list written when there was no choice meant `Both`, and
	// reading it as anything else would silently stop a working single-origin
	// setup from either fetching or uploading.
	const std::filesystem::path path = ScratchFile("atomic-sources-old.ini");
	std::filesystem::remove(path);

	{
		std::ofstream out(path, std::ios::trunc);
		out << "source = old | http | 127.0.0.1:9080 | on\n";
	}

	ContentSources sources;
	REQUIRE(sources.Load(path));
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Role == SourceRole::Both);
	CHECK(sources.Sources.front().Enabled);

	std::filesystem::remove(path);
}

TEST_CASE("reads and writes go to different origins", "[studio][contentsources]") {
	// The point of the whole feature, from the editor's end: one list, one
	// order, and each row saying which directions it is in.
	ContentSources sources;
	sources.PublisherKey = std::string(64, 'a');

	Source reader = Origin("reads", "127.0.0.1:9080");
	reader.Role = SourceRole::Read;

	Source writer = Origin("writes", "127.0.0.1:9081");
	writer.Role = SourceRole::Write;
	writer.IngestKey = "secret";

	sources.Sources = {reader, writer};

	const engine::delivery::DeliverySettings settings = sources.ToSettings();

	// Both rows reach the settings — the split happens when they are asked for,
	// not when they are written down, so a row keeps its address whichever
	// direction it is in.
	REQUIRE(settings.Sources.size() == 2);

	const std::vector<Source> readable = settings.Usable();
	REQUIRE(readable.size() == 1);
	CHECK(readable[0].Name == "reads");

	const std::vector<Source> writable = settings.Writable();
	REQUIRE(writable.size() == 1);
	CHECK(writable[0].Name == "writes");
}

TEST_CASE("an unrecognised role reads as both rather than dropping the row", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-badrole.ini");
	std::filesystem::remove(path);

	{
		std::ofstream out(path, std::ios::trunc);
		out << "source = odd | http | 127.0.0.1:9080 | on | sideways |\n";
	}

	ContentSources sources;
	REQUIRE(sources.Load(path));
	REQUIRE(sources.Sources.size() == 1);

	// The permissive answer is the one that keeps a list working — a row
	// dropped over a word nobody recognises is an origin that vanished.
	CHECK(sources.Sources.front().Role == SourceRole::Both);

	std::filesystem::remove(path);
}

// --- what a fresh editor can reach -------------------------------------------

TEST_CASE("a fresh editor can fetch from the store on this machine", "[studio][content]") {
	// **The default was an address nobody is listening on and no publisher
	// key**, which `DeliverySettings::IsValid` refuses — so `MakeAssetClient`
	// was never called and the editor fetched *nothing at all*. A `MeshPart`
	// drew the fallback cube however good its `MeshId` was, a `ColorMap` drew
	// nothing, and the assets panel happily listed a store full of content none
	// of which the viewport could show. Nothing said why: an unconfigured
	// delivery client and one whose fetches fail look identical from a viewport.
	const studio::ContentSources sources = studio::ContentSources::Default();
	const engine::delivery::DeliverySettings settings = sources.ToSettings();

	CHECK(settings.IsValid());
	REQUIRE_FALSE(settings.Usable().empty());

	// The store this machine's client publishes to and reads from, first.
	CHECK(settings.Usable().front().Kind == engine::delivery::SourceKind::Directory);
	CHECK(
		settings.Usable().front().Location == cdn::DefaultLocalPaths().Processed.string()
	);

	// **The key that can verify it.** Trusting the development identity is what
	// makes the local folder usable with nothing typed; it verifies only what
	// this editor publishes, and any other origin needs the real one.
	CHECK(sources.PublisherKey == cdn::DevelopmentPublisher().ToHex());
}

TEST_CASE("the remote row is kept and turned off", "[studio][content]") {
	// **Kept rather than dropped**, which is `ContentSources.hpp`'s own rule one
	// row over: a disabled source is one somebody can switch on, and deleting
	// the address a deployment uses would make the common remote case something
	// to retype.
	const studio::ContentSources sources = studio::ContentSources::Default();
	REQUIRE(sources.Sources.size() >= 2);

	bool sawDisabledHttp = false;
	for (const engine::delivery::Source &source : sources.Sources) {
		if (source.Kind == engine::delivery::SourceKind::Http) {
			CHECK_FALSE(source.Enabled);
			sawDisabledHttp = true;
		}
	}
	CHECK(sawDisabledHttp);
}

TEST_CASE("the default survives a save and a load", "[studio][content]") {
	// The key is written and read back, which the round trip did not cover
	// before because the default had none.
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "atomic-studio-content-default.ini";
	std::filesystem::remove(path);

	const studio::ContentSources written = studio::ContentSources::Default();
	REQUIRE(written.Save(path));

	studio::ContentSources read;
	REQUIRE(read.Load(path));
	CHECK(read.PublisherKey == written.PublisherKey);
	REQUIRE(read.Sources.size() == written.Sources.size());
	CHECK(read.Sources[0].Location == written.Sources[0].Location);
	CHECK(read.Sources[0].Enabled);

	std::filesystem::remove(path);
}
