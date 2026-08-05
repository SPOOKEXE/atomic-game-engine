#pragma once

// The studio's list of content origins, and the order they are tried in.
//
// **The order is the whole feature.** "Fetch from the local cache first,
// otherwise ask the origin next door, otherwise the one across the internet" is
// not a policy anything in the engine implements — it is what a list in that
// order *means* to `delivery::AssetClient`, which walks it and stops at the
// first source that answers. This file is the editor's way of writing that
// list down and keeping it between sessions.
//
// **Kept beside the binary with the layout ini and the keybinds**, for the same
// reason `Keybinds::Load` gives: a launcher's working directory must not decide
// whose configuration is loaded.
//
// **A row that is turned off is kept, not deleted.** Somebody working out which
// of three origins is broken wants to switch one off and back on, and a
// preferences page that forgot the address when it was disabled would make that
// a retyping exercise.
//
// @tier client

#include <engine/delivery/Source.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace studio {

	// The editor's content sources, in priority order.
	//
	// @since v0.9
	struct ContentSources {
		// The sources, first tried first.
		std::vector<engine::delivery::Source> Sources;

		// Where verified content is kept between runs. Empty disables it.
		std::filesystem::path CachePath;

		// The publisher key whose manifests this editor trusts, as 64 hex
		// characters. Empty means delivery is not configured.
		std::string PublisherKey;

		// The list a fresh install starts with: one origin, on this machine.
		//
		// @return Sources naming `127.0.0.1:9080` and nothing else.
		static ContentSources Default();

		// Turns these into what a delivery client takes.
		//
		// @return The settings. Not necessarily valid — an editor with no
		//         publisher key configured has a list and no trust, and
		//         `MakeAssetClient` is the one that refuses.
		engine::delivery::DeliverySettings ToSettings() const;

		// Writes the list.
		//
		// @param path Where to write it.
		// @return Whether it was written.
		bool Save(const std::filesystem::path &path) const;

		// Reads what Save wrote.
		//
		// A missing file is **not** an error: a fresh install has none and gets
		// the default, which is what somebody expects the first time they open
		// the editor.
		//
		// @param path Where to read from.
		// @return Whether a file was read.
		bool Load(const std::filesystem::path &path);

		// Moves a row up or down, which is how priority is edited.
		//
		// @param index The row to move.
		// @param delta -1 for earlier, +1 for later.
		// @return Whether anything moved.
		bool Move(size_t index, int delta);
	};
}
