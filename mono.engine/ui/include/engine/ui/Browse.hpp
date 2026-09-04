#pragma once

// arch-waiver public-header: forward UI API. Interface hosts use this complete
// browse-dialog contract without owning platform selection policy.

// Listing a directory, for the file dialogs.
//
// **Here rather than in `mono.studio`, since v0.18.** The editor wrote this and
// was its only caller until `mono.launcher` needed to pick a game file, a
// content store and a cache directory out of the same tree. What the two share
// is not a widget - one browses in a modal over a docked editor, the other in a
// launcher form - it is the *rules*: what counts as hidden, how rows sort, that
// an unreadable entry is skipped rather than fatal, and that a suffix filter is
// case-insensitive. Those are one fact, and rule 2 is about one fact having one
// home.
//
// **An imgui browser over `std::filesystem` rather than a vendored native
// dialog**, and the reason is not that a native one would be hard. `mono.vendor`
// holds nothing for this, and every candidate brings a platform surface with it:
// a second event loop on one platform, a portal round trip on another, a
// different set of failure modes on each. The editor is already imgui, the
// browser is a table and a path bar, and it behaves identically everywhere the
// studio builds.
//
// The trade being accepted, said out loud: it will not look like the host's file
// dialog, and it will not have the host's places sidebar or its recent-files
// list. What it does have is the thing that was actually missing - until v0.7
// every one of Open, Save As, Import World, Import Universe, Export World and
// Export Universe asked for a full path to be typed, with no browsing, no
// completion and no validation until the button was pressed.
//
// This header is the half with no imgui in it, so it can be tested.
//
// @tier L12 · client

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::ui {

	// One row in a listing.
	//
	// @since v0.7
	struct BrowseEntry {
		// What to show. The file name only, never the whole path - the path bar
		// above the list already says where you are.
		std::string Name;

		// Where it actually is, which is what a caller acts on.
		std::filesystem::path Path;

		// Whether descending into it is the thing to do.
		bool Directory = false;
	};

	// What one directory looks like to the browser.
	//
	// @since v0.7
	struct Listing {
		// Where this is a listing of.
		std::filesystem::path Directory;

		// The parent, or empty at a filesystem root. Held rather than computed
		// at the call site so that "up" is a field and not a special case: at a
		// root, `parent_path()` returns the root again, and a caller comparing
		// them itself is a caller that will forget to.
		std::filesystem::path Parent;

		// Directories first, then files, each sorted by name. The rows.
		std::vector<BrowseEntry> Entries;

		// Why the listing is empty, when it is empty because of a failure
		// rather than because the directory is. Empty otherwise.
		std::string Error;
	};

	// Lists a directory for the browser.
	//
	// **Unreadable entries are skipped rather than aborting the listing.** A
	// directory containing one file whose permissions cannot be read is an
	// ordinary situation, and a browser that showed nothing at all because of it
	// would be a browser that fails in exactly the places somebody most needs to
	// look around.
	//
	// Hidden entries - a leading dot - are omitted. Neither caller's paths need
	// them and a first listing full of `.git` and `.cache` is a listing nobody
	// can find anything in.
	//
	// @param directory Where to list. A path that is not a directory lists its
	//                  parent, so a browser opened on a file's path shows the
	//                  file's folder rather than an error.
	// @param extensions Which file suffixes to show, lowercase and with the dot
	//                   - `{".agame"}`. Empty shows every file. Directories are
	//                   never filtered out, or there would be no way to reach a
	//                   folder containing what you want.
	// @return The listing. Check `Error` for why an empty one is empty.
	Listing
	BrowseDirectory(const std::filesystem::path &directory, const std::vector<std::string> &extensions = {});

	// Whether a name matches a suffix filter, case-insensitively.
	//
	// **Case-insensitive because a file is not.** `MyGame.AGAME` is a game file,
	// and a filter that hid it would be a filter that hides the file somebody is
	// looking straight at.
	//
	// @param name       The file name.
	// @param extensions The suffixes, lowercase and with the dot. Empty matches
	//                   everything.
	// @return `true` when it should be shown.
	bool MatchesExtension(std::string_view name, const std::vector<std::string> &extensions);
}
