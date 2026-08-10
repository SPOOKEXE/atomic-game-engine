// The studio's configuration folder.
//
// **Every case points the root at a scratch directory first.** A suite that
// wrote into `~/Documents/atomic-game-engine/studio` would overwrite a real
// person's preferences the first time anybody ran it, which is why
// `SetConfigRoot` exists at all.
//
// What is worth asserting is not that a round trip works — it is the behaviour
// around the edges, because those are what a person meets: a fresh install with
// no files, a file somebody edited by hand and broke, and a recent list that has
// to stay in "most recent first" order across a save and a load.

#include <engine/testing/Suite.hpp>
#include <studio/Config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_ID("studio.config")

using nlohmann::json;
using studio::ConfigPath;
using studio::ConfigRoot;
using studio::Preferences;
using studio::ReadConfigDocument;
using studio::RecentProjects;
using studio::SetConfigRoot;
using studio::WriteConfigDocument;

namespace {
	// A scratch config folder, restored on the way out so one case cannot
	// decide where the next one writes.
	struct Scratch {
		std::filesystem::path Root;

		Scratch() {
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-studio-config-" +
					std::to_string(std::filesystem::hash_value(
						std::filesystem::temp_directory_path() / "studio-config"
					)));
			std::filesystem::remove_all(Root);
			SetConfigRoot(Root);
		}

		~Scratch() {
			SetConfigRoot({});
			std::filesystem::remove_all(Root);
		}

		void Write(const char *leaf, const char *text) {
			std::filesystem::create_directories(Root);
			std::ofstream out(Root / leaf);
			out << text;
		}
	};
}

TEST_CASE("the root is overridable and every path derives from it", "[studio][config]") {
	Scratch scratch;

	CHECK(ConfigRoot() == scratch.Root);
	CHECK(ConfigPath("preferences.json") == scratch.Root / "preferences.json");
	CHECK(ConfigPath("recent.json").parent_path() == scratch.Root);

	// The default is somewhere under a home directory, and naming the exact
	// path here would pin an environment rather than the behaviour.
	SetConfigRoot({});
	CHECK(ConfigRoot().filename() == "studio");
	CHECK(ConfigRoot().parent_path().filename() == "atomic-game-engine");
}

TEST_CASE("a missing document is not an error", "[studio][config]") {
	Scratch scratch;

	// A fresh install has none of these. A caller has to be able to tell that
	// from a file it could not read, which is what an empty error says.
	json document;
	std::string error;
	CHECK_FALSE(ReadConfigDocument("preferences.json", document, error));
	CHECK(error.empty());

	Preferences preferences;
	CHECK_FALSE(preferences.Load());

	RecentProjects recent;
	CHECK_FALSE(recent.Load());
	CHECK(recent.Paths.empty());
}

TEST_CASE("a document that is not JSON says so once", "[studio][config]") {
	Scratch scratch;
	scratch.Write("preferences.json", "{ this is not json");

	json document;
	std::string error;
	CHECK_FALSE(ReadConfigDocument("preferences.json", document, error));
	CHECK_FALSE(error.empty());

	// And the defaults survive it. An editor that quietly forgot every
	// preference with no line anywhere saying why is the failure this is
	// written against.
	Preferences preferences;
	preferences.SnapDistance = 4.0f;
	CHECK_FALSE(preferences.Load());
	CHECK(preferences.SnapDistance == 4.0f);
}

TEST_CASE("writing a document creates the folder", "[studio][config]") {
	Scratch scratch;
	REQUIRE_FALSE(std::filesystem::exists(scratch.Root));

	std::string error;
	REQUIRE(WriteConfigDocument("thing.json", json{{"value", 7}}, error));
	CHECK(error.empty());
	CHECK(std::filesystem::is_regular_file(scratch.Root / "thing.json"));

	json read;
	REQUIRE(ReadConfigDocument("thing.json", read, error));
	CHECK(read["value"] == 7);
}

TEST_CASE("preferences round trip and are read forward", "[studio][config]") {
	Scratch scratch;

	Preferences written;
	written.Scale = 1.25f;
	written.ShowGrid = false;
	written.SnapEnabled = true;
	written.SnapDistance = 0.25f;
	written.SnapDegrees = 45.0f;
	written.PivotEditing = true;
	written.ControlPort = 9001;
	written.ShowControl = true;
	written.Sides = studio::ScaleSide::BothHalf;
	REQUIRE(written.Save());

	Preferences read;
	REQUIRE(read.Load());
	CHECK(read.Scale == 1.25f);
	CHECK_FALSE(read.ShowGrid);
	CHECK(read.SnapEnabled);
	CHECK(read.SnapDistance == 0.25f);
	CHECK(read.SnapDegrees == 45.0f);
	CHECK(read.PivotEditing);
	CHECK(read.ControlPort == 9001);
	CHECK(read.ShowControl);

	// **Written as its name rather than its index**, so reordering `ScaleSide`
	// cannot silently change how everybody's scale drag behaves. A name nobody
	// knows leaves the default alone rather than landing on whichever member
	// happens to be first.
	CHECK(read.Sides == studio::ScaleSide::BothHalf);

	scratch.Write("preferences.json", R"({"scaleSides": "Sideways"})");
	Preferences unknown;
	unknown.Sides = studio::ScaleSide::Both;
	REQUIRE(unknown.Load());
	CHECK(unknown.Sides == studio::ScaleSide::Both);

	// **A document written by an older build leaves what it does not mention
	// alone**, which is what makes adding a field a change rather than a
	// migration.
	scratch.Write("preferences.json", R"({"gridStep": 2.0})");

	Preferences partial;
	partial.SnapDegrees = 30.0f;
	REQUIRE(partial.Load());
	CHECK(partial.SnapDistance == 2.0f);
	CHECK(partial.SnapDegrees == 30.0f);
}

TEST_CASE("a panel's colours round trip, and a broken one is skipped", "[studio][config]") {
	Scratch scratch;

	// **The one preference that is a document rather than a number**, and the
	// only one somebody would plausibly hand-edit a colour into — so what is
	// worth asserting is that a name nobody knows, a colour that is not one,
	// and a panel with nothing left in it each cost only themselves.
	Preferences written;
	written.PanelColours["Explorer"][engine::ui::ThemeColour::Surface] =
		IM_COL32(0x2E, 0x34, 0x40, 0xFF);
	written.PanelColours["Output"][engine::ui::ThemeColour::Accent] =
		IM_COL32(0xFF, 0x00, 0x80, 0xC0);
	REQUIRE(written.Save());

	Preferences read;
	REQUIRE(read.Load());
	REQUIRE(read.PanelColours.count("Explorer") == 1);
	CHECK(
		read.PanelColours["Explorer"][engine::ui::ThemeColour::Surface] ==
		IM_COL32(0x2E, 0x34, 0x40, 0xFF)
	);
	CHECK(
		read.PanelColours["Output"][engine::ui::ThemeColour::Accent] == IM_COL32(0xFF, 0x00, 0x80, 0xC0)
	);

	// Nothing else was pinned by writing one colour, which is what keeps a
	// recoloured panel following the theme in every other respect.
	CHECK_FALSE(read.PanelColours["Explorer"][engine::ui::ThemeColour::Accent].has_value());

	// Written the way a person reads a colour, not the way imgui packs one.
	json document;
	std::string error;
	REQUIRE(studio::ReadConfigDocument("preferences.json", document, error));
	CHECK(document["panelColours"]["Explorer"]["Surface"] == "2E3440FF");

	scratch.Write(
		"preferences.json",
		R"({"panelColours": {
			"Explorer": {"Surface": "#1A1A1A", "Sparkle": "FFFFFFFF", "Accent": "not a colour"},
			"Ghost": {"Accent": "zzz"},
			"Empty": {}
		}})"
	);

	Preferences salvaged;
	REQUIRE(salvaged.Load());
	CHECK(
		salvaged.PanelColours["Explorer"][engine::ui::ThemeColour::Surface] ==
		IM_COL32(0x1A, 0x1A, 0x1A, 0xFF)
	);
	CHECK_FALSE(salvaged.PanelColours["Explorer"][engine::ui::ThemeColour::Accent].has_value());

	// A panel whose every line was unreadable is not a panel — an entry with
	// nothing in it would be written straight back out on the next save.
	CHECK(salvaged.PanelColours.count("Ghost") == 0);
	CHECK(salvaged.PanelColours.count("Empty") == 0);
}

TEST_CASE("a hand-edited preference is clamped rather than obeyed", "[studio][config]") {
	Scratch scratch;

	// Every one of these is one typo away in a file this format exists to let
	// somebody edit. A scale of zero is a window nobody can read.
	scratch.Write(
		"preferences.json",
		R"({"scale": 0.0, "gridStep": -4.0, "rotationStep": -1.0, "controlPort": 99999})"
	);

	Preferences preferences;
	REQUIRE(preferences.Load());
	CHECK(preferences.Scale >= 0.5f);

	// A step of zero would round every drag onto one point. Snapping is turned
	// off with the checkbox, never by writing nothing in the box.
	CHECK(preferences.SnapDistance > 0.0f);
	CHECK(preferences.SnapDegrees > 0.0f);
	CHECK(preferences.ControlPort == 65535);
}

TEST_CASE("a field of the wrong type falls back rather than throwing", "[studio][config]") {
	Scratch scratch;
	scratch.Write("preferences.json", R"({"gridStep": "half a stud", "showGrid": 1})");

	Preferences preferences;
	preferences.SnapDistance = 3.0f;
	preferences.ShowGrid = true;

	REQUIRE(preferences.Load());
	CHECK(preferences.SnapDistance == 3.0f);
	CHECK(preferences.ShowGrid);
}

TEST_CASE("the recent list keeps the newest first and drops the oldest", "[studio][config]") {
	Scratch scratch;
	RecentProjects recent;

	for (int index = 1; index <= 7; index++) {
		recent.Remember("game" + std::to_string(index) + ".agame");
	}

	REQUIRE(recent.Paths.size() == RecentProjects::LIMIT);
	CHECK(recent.Paths.front() == "game7.agame");
	CHECK(recent.Paths.back() == "game3.agame");

	// **Moved rather than added when it is already there**, which is what "most
	// recent" means — otherwise opening one file five times would fill the menu
	// with five copies of it.
	recent.Remember("game3.agame");
	CHECK(recent.Paths.size() == RecentProjects::LIMIT);
	CHECK(recent.Paths.front() == "game3.agame");

	// An empty path is not a project to return to.
	recent.Remember({});
	CHECK(recent.Paths.size() == RecentProjects::LIMIT);

	recent.Forget("game3.agame");
	CHECK(recent.Paths.size() == RecentProjects::LIMIT - 1);
	CHECK(recent.Paths.front() != "game3.agame");
}

TEST_CASE("the recent list survives a save and a load in order", "[studio][config]") {
	Scratch scratch;

	RecentProjects written;
	written.Remember("first.agame");
	written.Remember("second.agame");
	written.Remember("third.agame");
	REQUIRE(written.Save());

	RecentProjects read;
	REQUIRE(read.Load());
	REQUIRE(read.Paths.size() == 3);
	CHECK(read.Paths[0] == "third.agame");
	CHECK(read.Paths[1] == "second.agame");
	CHECK(read.Paths[2] == "first.agame");
}

TEST_CASE("a hand-edited recent list comes back obeying the same rules", "[studio][config]") {
	Scratch scratch;

	// Seven entries with a duplicate among them, which is more than a session
	// could have produced.
	scratch.Write(
		"recent.json",
		R"({"projects": ["a", "b", "c", "d", "e", "f", "b", 7]})"
	);

	RecentProjects recent;
	REQUIRE(recent.Load());
	CHECK(recent.Paths.size() == RecentProjects::LIMIT);

	// The duplicate moved to the front of the file's order, and the number was
	// skipped rather than read as a path.
	CHECK(recent.Paths.front() == "a");
	for (const std::filesystem::path &path : recent.Paths) {
		CHECK_FALSE(path.empty());
	}
}
