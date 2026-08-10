#pragma once

// Where the studio keeps what a person configured, and how it reads and writes
// it.
//
// **One folder, outside the build, with one file per thing.**
//
//     ~/Documents/atomic-game-engine/studio/
//       preferences.json   what the interface looks like and how it behaves
//       cdn.json           the content origins and the order they are tried
//       recent.json        the last five projects opened
//       keybinds.json      what every action is bound to
//
// Three decisions, and each replaced something worse:
//
// **Beside the content store, not beside the binary.** `studio-layout.ini`,
// `studio-content.ini` and `studio-keybinds.ini` lived in `Paths::Base()` —
// which is `.cache/build/<preset>/` for anybody working on the engine. So every
// `just build` against a different preset was a fresh install, and deleting the
// build directory threw away somebody's keybinds and their origin list. The CDN
// half already lives in `~/Documents/atomic-game-engine`; this is the other
// half of the same folder.
//
// **JSON rather than INI**, because these documents are no longer flat. A
// recent-projects list is an array and a source list is an array of records;
// the INI writer already had to invent an index-in-the-key convention to say so,
// and the reader had to parse it back. Every other document this repository
// writes for a person to read is JSON.
//
// **A read never fails for a missing file.** A fresh install has none of these,
// and that is the ordinary case rather than an error — every `Load` here answers
// `false` and leaves the caller's defaults alone, so nothing has to distinguish
// "not configured yet" from "configured to the default".
//
// ## What belongs here and what does not
//
// A preference is a thing **somebody set and expects to find again**. A command
// line flag is a thing they said *for one run*, so a flag always wins over the
// file and the file is never written from one — otherwise `--headless` once
// would make an editor headless forever.
//
// A signing seed does not belong here for the reason `ContentSources.hpp` gives
// at length: a seed sitting in a preferences file signs anything anybody drops
// into the content folder.
//
// @tier client

#include <cstddef>
#include <engine/ui/Theme.hpp>
#include <filesystem>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace studio {

	// The folder every document below lives in.
	//
	// `~/Documents/atomic-game-engine/studio`, or the working directory when
	// there is no home to find — which is a container or a service account
	// rather than a person, and neither is a case to abort in.
	//
	// @return The directory, which may not exist yet.
	// @since v0.12
	const std::filesystem::path &ConfigRoot();

	// Points the config folder somewhere else.
	//
	// **For tests, and for a second editor on one machine.** Every path below is
	// derived from the root rather than computed, so this is the only override
	// there needs to be — and a suite that wrote into a real person's folder
	// would be a suite nobody could run twice.
	//
	// @param root Where to keep configuration. An empty path restores the
	//             default.
	// @since v0.12
	void SetConfigRoot(const std::filesystem::path &root);

	// One document's path.
	//
	// @param leaf The file name, `preferences.json` and the rest.
	// @return `ConfigRoot() / leaf`.
	// @since v0.12
	std::filesystem::path ConfigPath(std::string_view leaf);

	// Creates the config folder if it is not there.
	//
	// @return `false` when it could not be created, which is reported once by
	//         whichever write found out.
	// @since v0.12
	bool EnsureConfigRoot();

	// Reads one document.
	//
	// **A missing file is `false` with an empty `error`**, which is what lets a
	// caller tell "nothing configured" from "configured and unreadable" without
	// checking the filesystem itself. A malformed one fills `error` and is
	// never half-applied.
	//
	// @param leaf     The file name.
	// @param out      Filled on success.
	// @param error    Filled when the file is there and cannot be read.
	// @return `true` when a document was read.
	// @since v0.12
	bool ReadConfigDocument(std::string_view leaf, nlohmann::json &out, std::string &error);

	// Writes one document, creating the folder if it has to.
	//
	// Pretty-printed, because every one of these is a file somebody may open in
	// an editor and fix by hand — which is most of the reason for JSON over a
	// binary form.
	//
	// @param leaf     The file name.
	// @param document What to write.
	// @param error    Filled on failure.
	// @return `true` when it was written.
	// @since v0.12
	bool WriteConfigDocument(std::string_view leaf, const nlohmann::json &document, std::string &error);

	// The last few games opened, most recent first.
	//
	// @since v0.12
	struct RecentProjects {
		// How many are kept. The roadmap's number, and it is the right shape for
		// a menu: a list somebody scans rather than searches.
		static constexpr size_t LIMIT = 5;

		// The paths, most recent first. Never longer than `LIMIT`.
		std::vector<std::filesystem::path> Paths;

		// Puts one at the front.
		//
		// **Moved rather than added when it is already in the list**, which is
		// what "most recent" means — otherwise opening one file five times would
		// fill the menu with five copies of it and push out everything else.
		//
		// An empty path is ignored, because "no file" is the state a new game is
		// in and it is not a thing to return to.
		//
		// @param path The game file just opened or saved.
		void Remember(const std::filesystem::path &path);

		// Drops one, for a path that no longer exists.
		//
		// **Not done automatically on load**, and that is deliberate: a project
		// on a drive that is not plugged in right now is still a project
		// somebody wants in the list, and a menu that silently forgot it would
		// be worse than one with a row that says it is missing.
		//
		// @param path The entry to remove.
		void Forget(const std::filesystem::path &path);

		// Reads `recent.json`.
		//
		// @return `false` when there is nothing to read, leaving this empty.
		bool Load();

		// Writes `recent.json`.
		//
		// @return `false` when it could not be written.
		bool Save() const;
	};

	// What the interface looks like and how it behaves, between sessions.
	//
	// **Deliberately not `EditorSettings`.** That struct is the command line —
	// what somebody asked for on one run — and this is what they configured. The
	// two overlap on purpose in a few places and the rule at every one of them
	// is the same: a flag given wins, and the file is never written from a flag.
	//
	// @since v0.12
	struct Preferences {
		// The interface's scale, as `--scale` sets it for one run.
		float Scale = 1.0f;

		// Whether the ground grid is drawn.
		bool ShowGrid = true;

		// Whether a dragged handle snaps at all.
		//
		// **These three are `Editor::SnapEnabled`, `SnapDistance` and
		// `SnapDegrees`, spelled the same and holding the same numbers.** They
		// are here rather than duplicated because a step is the first thing
		// anybody changes and the last thing they want to set again tomorrow —
		// and a second pair of fields meaning the same thing is the debt the
		// root `AGENTS.md` calls the most expensive kind.
		bool SnapEnabled = false;

		// How far a dragged handle steps, in studs.
		float SnapDistance = 1.0f;

		// How far a rotation handle steps, in degrees.
		float SnapDegrees = 15.0f;

		// Whether a handle edits the pivot offset rather than the placement.
		//
		// **A mode, because the handles are the same ones.** Roblox spells it
		// "Edit Pivot" and it is the same translate and rotate gizmo pointed at
		// `PivotOffset` — see `Editor::PivotEditing`.
		bool PivotEditing = false;

		// The loopback port the control surface offers in its panel.
		//
		// **Not whether it is listening.** A port is a preference and an open
		// socket is a decision somebody makes while working — `SECURITY.md` is
		// why the second is never restored from a file.
		int ControlPort = 8720;

		// Which of the panels somebody keeps open.
		//@{
		bool ShowStatistics = false;
		bool ShowFrameGraph = false;
		bool ShowAssets = false;
		bool ShowControl = false;
		//@}

		// What a panel was coloured, keyed by the title imgui identifies it with.
		//
		// **Here rather than in the layout ini, unlike the global theme.** The
		// global override is one line of numbers and belongs beside the palette
		// it overrides; this is a document — a map of maps that grows with every
		// panel somebody recolours — and the header above is explicit that JSON
		// is where documents go and the INI convention for saying so was the
		// thing that replaced.
		//
		// **Absent means "the theme", not "black".** A panel with no entry draws
		// exactly as it did before anybody could colour one, which is what keeps
		// this feature something somebody opts into per panel rather than a
		// second set of defaults to maintain.
		//
		// @since v0.13
		std::map<std::string, engine::ui::ThemeColours> PanelColours;

		// Reads `preferences.json`, leaving anything it does not mention alone.
		//
		// @return `false` when there is nothing to read.
		bool Load();

		// Writes `preferences.json`.
		//
		// @return `false` when it could not be written.
		bool Save() const;
	};
}
