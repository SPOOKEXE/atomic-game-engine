#pragma once

// The two modal dialogs that ask for a path, over `ui::Browse`'s listing.
//
// **Here rather than in `mono.studio`, since v0.18.** The editor wrote both and
// was their only caller until `mono.launcher` arrived - and a launcher whose
// every `PATH` and `DIR` option offers a browse button is a program that needs
// exactly these two and nothing else studio's `Widgets.hpp` holds. Copying them
// would have put a second file dialog in the repository with a second set of
// keyboard behaviours, which is the thing `studio/Widgets.hpp` already says out
// loud it is trying to avoid within one program.
//
// **Three functions rather than one with two flags.** A file dialog returns what
// is selected in the list and a folder dialog returns where the list *is*.
// Folded together, a click on a row would mean "descend" in one mode and
// "choose" in the other, from the same widget - which is how a dialog grows a
// mode nobody can see. `FoldersPrompt` splits again for the same reason: with
// several answers a row needs a tick box beside the name, and the name itself
// goes back to meaning "descend".
//
// Both follow imgui's popup contract: the caller calls `ImGui::OpenPopup` with
// the same title, calls this every frame, and acts on `path` on the frame it
// returns true.
//
// @tier L12 · client

#include <string>
#include <vector>

namespace engine::ui {

	// A modal that browses for a file, rather than asking for one to be typed.
	//
	// @param title      The popup id, which is also its heading.
	// @param path       The path, in and out. Its folder is where browsing
	//                   starts.
	// @param accept     What the confirm button says.
	// @param extensions Which suffixes to list, lowercase and with the dot.
	//                   Empty lists every file.
	// @param mustExist  Whether the accept button refuses a path that is not
	//                   there. Open refuses; Save As does not, because naming a
	//                   file that does not exist yet is the whole point of it.
	// @return `true` on the frame it is confirmed.
	// @since v0.7
	bool FilePrompt(
		const char *title,
		std::string &path,
		const char *accept,
		const std::vector<std::string> &extensions,
		bool mustExist
	);

	// A modal that browses for a *folder* rather than a file.
	//
	// Files are listed, greyed, and not selectable - a folder browser that hid
	// them would make it impossible to tell an empty folder from the right one.
	//
	// @param title  The popup id, which is also its heading.
	// @param path   The folder, in and out. Where browsing starts.
	// @param accept What the confirm button says.
	// @return `true` on the frame it is confirmed.
	// @since v0.10
	bool FolderPrompt(const char *title, std::string &path, const char *accept);

	// A modal that browses for *several* folders at once.
	//
	// **The ticks survive walking into another parent**, which is the whole
	// reason this is not "open the single dialog three times": the folders
	// somebody wants are rarely siblings, and a dialog that forgot the first
	// pick the moment you went looking for the second would be slower than
	// three separate trips rather than faster.
	//
	// The name descends and the tick box beside it selects, so neither click
	// has to guess which the person meant. The confirm button carries the count,
	// because a selection made three directories ago is otherwise invisible.
	//
	// @param title  The popup id, which is also its heading.
	// @param path   Where browsing starts, and where it ended. In and out.
	// @param chosen Filled in with the ticked folders on the frame this returns
	//               true. Untouched otherwise, and cleared before it is filled.
	// @param accept What the confirm button says, before its count.
	// @return `true` on the frame it is confirmed with at least one tick.
	// @since v0.18
	bool
	FoldersPrompt(const char *title, std::string &path, std::vector<std::string> &chosen, const char *accept);
}
