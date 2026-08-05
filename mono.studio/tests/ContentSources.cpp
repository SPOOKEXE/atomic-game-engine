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
#include <studio/ContentSources.hpp>

TEST_SUITE_ID("studio.contentsources")

using engine::delivery::Source;
using engine::delivery::SourceKind;
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

TEST_CASE("a fresh install starts with one local origin", "[studio][contentsources]") {
	const ContentSources sources = ContentSources::Default();

	// One, not none: an editor that came up with an empty list would report
	// "no sources" for a machine that has an origin running next to it, which
	// is the ordinary development arrangement.
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Enabled);

	// And no trust by default. The publisher key is the thing a person has to
	// supply, and defaulting one would be defaulting who this editor believes.
	CHECK(sources.PublisherKey.empty());
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
