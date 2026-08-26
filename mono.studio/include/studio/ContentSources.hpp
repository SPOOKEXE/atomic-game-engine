#pragma once

// The studio's list of content origins, and the order they are tried in.
//
// **The order is the whole feature.** "Fetch from the local cache first,
// otherwise ask the origin next door, otherwise the one across the internet" is
// not a policy anything in the engine implements - it is what a list in that
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
// **Each row also says which direction it is used in** - `delivery::SourceRole`
// - which is what lets one origin take every write and a different one serve
// every read. The order is still the priority, and it is one order rather than
// two: what somebody wants in both directions is nearest first, and two lists
// that could drift apart would be two chances to configure a mirror wrong.
//
// **The ingest key is saved here and the signing seed is not, and that is not
// an inconsistency.** A signing seed decides what a *client* will trust, so one
// sitting in a preferences file is a key that signs anything anybody drops in
// the content folder - `cdn::PublishLocal` and `DrawAssets` both refuse to keep
// one. An ingest key only decides who may spend a write origin's disk: content
// that reaches an inbox is still unsigned, and no client looks at it until a
// publisher has signed a manifest naming it. Losing it costs disk, not trust,
// and an upload target somebody has to retype every session is one they will
// stop using. `cdn::IngestSettings` carries the argument in full.
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

		// Folders of unprocessed art this editor may bake from directly.
		//
		// **Not `Sources`, and the separation is the point.** A `Source` is a
		// place `delivery::AssetClient` fetches from, and everything it fetches
		// is named by a signed manifest, which is what makes it trustworthy at
		// all. A folder
		// of PNGs has no manifest and no signature, so admitting one as a source
		// would mean either a client that trusts a directory or a manifest
		// somebody has to fabricate. Neither is worth having.
		//
		// What this is instead is an **authoring** convenience: the editor bakes
		// what it needs, when it needs it, and registers the result into its own
		// tables. Nothing reaches a client from here until it has been imported,
		// published and signed like everything else - and the assets panel says
		// so on the tab.
		//
		// @since v0.14
		std::vector<std::filesystem::path> RawFolders;

		// Whether baking from a raw folder keeps the result in memory.
		//
		// **On by default, because the folder is somebody's own art directory.**
		// Baking a copy of it into their content store the first time they look
		// at a texture is a side effect they did not ask for and would have to
		// clean up - and the common case for pointing at a raw folder is
		// *looking*, not shipping. Turning it off writes each baked asset into
		// the local store's `baked/`, which is where a publish reads from.
		//
		// @since v0.14
		bool MemoryOnly = true;

		// The list a fresh install starts with: one origin, on this machine.
		//
		// @return Sources naming `127.0.0.1:9080` and nothing else.
		static ContentSources Default();

		// Turns these into what a delivery client takes.
		//
		// @return The settings. Not necessarily valid - an editor with no
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
