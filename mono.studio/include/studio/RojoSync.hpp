#pragma once

// Reading a Rojo project, and building the tree it describes.
//
// **The format is somebody else's and that is the point.** `default.project.json`
// is what the Roblox ecosystem already writes: a `$className`/`$path` tree
// mapping folders on disk onto instances in a place. A game written against
// Rojo has its source laid out that way, its tooling assumes it, and every
// author who has used Roblox knows it. Inventing a second layout would ask them
// to convert a working project in order to try this engine, which is the wrong
// side of the trade for a format that costs a parser to read.
//
// ## The mapping, said once
//
// | On disk | In the world |
// |---|---|
// | a directory | a `Folder` |
// | `X.luau` | a `Script` named `X` |
// | `X.server.luau` | a `Script` named `X` |
// | `X.client.luau` | a `LocalScript` named `X` |
// | `init.luau` in `D/` | `D` itself becomes the script, keeping its children |
//
// `init.luau` is the one that looks like a special case and is not: Rojo uses it
// so a script can have children, which is exactly what an instance tree is for.
// Without it a module with sub-modules would have to be a folder *beside* a
// script, and every path in the project would gain a level.
//
// ## What this does not do
//
// **It builds, it does not watch.** A file watcher is a thread, a debounce and a
// decision about what happens when a sync lands mid-tick — and the last of those
// is rule 5's question, not a detail. Syncing on demand answers the same need
// for an author who is editing in an external editor and pressing one key to see
// it, and leaves the live-sync design to be taken deliberately.
//
// **It reads, it does not write back.** Two-way sync means conflict semantics
// for every instance the editor can touch, which `docs/v07v08.md` files under
// v0.10 as a real piece of work rather than a flag.
//
// @tier client

#include <engine/ecs/Store.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace studio {

	// One node of a project's tree.
	//
	// @since v0.10
	struct RojoNode {
		// What the instance is called. The key it appeared under.
		std::string Name;

		// Its `$className`, or empty when the node only names a path.
		std::string ClassName;

		// Its `$path`, relative to the project file, or empty.
		std::string Path;

		// Nodes declared inside it, in the order they were written.
		//
		// **Order is kept rather than sorted**, because a project file is
		// authored and a tree that reordered itself would make two syncs of one
		// file produce two different creation orders — which is a different
		// entity id per instance and a recording that does not replay.
		std::vector<RojoNode> Children;
	};

	// A parsed `default.project.json`.
	//
	// @since v0.10
	struct RojoProject {
		// The project's `name`.
		std::string Name;

		// The `tree`, whose own name is the root's.
		RojoNode Tree;
	};

	// What a sync did.
	//
	// @since v0.10
	struct RojoSyncReport {
		// Instances created, of any kind.
		size_t Instances = 0;

		// Of those, how many carry a program.
		size_t Scripts = 0;

		// Paths named by the project that are not on disk.
		//
		// **Reported rather than fatal.** A project file commonly names
		// `Packages` before anything has installed one, and refusing to sync
		// the other nine tenths because of it would make the feature unusable
		// on a fresh clone.
		std::vector<std::string> Missing;

		// Anything the sync had to decide rather than read — a class this engine
		// does not have, a file it did not recognise. One line each, in the
		// words somebody can act on.
		std::vector<std::string> Notes;
	};

	// Parses a project file.
	//
	// @param json  The file's contents.
	// @param out   Filled on success.
	// @param error Filled on failure.
	// @return `false` when the document is not a project file.
	bool ParseRojoProject(std::string_view json, RojoProject &out, std::string &error);

	// Builds a project's tree into a world.
	//
	// **Additive, and it says what it created.** Nothing is deleted: a sync that
	// removed whatever it did not recognise would be a sync that ate an author's
	// hand-placed scene the first time they ran it against a partial project.
	//
	// @param project The parsed project.
	// @param root    The directory the project file sits in. `$path` is relative
	//                to it.
	// @param store   The world to build into.
	// @param report  Filled with what happened.
	// @param error   Filled on failure.
	// @return `false` when nothing could be built.
	bool SyncRojoProject(
		const RojoProject &project,
		const std::filesystem::path &root,
		engine::ecs::Store &store,
		RojoSyncReport &report,
		std::string &error
	);

	// The class every plain directory becomes.
	//
	// **Registered here rather than in `scene`, because it is this feature that
	// needs it.** A folder is an instance with no components and no behaviour —
	// it exists to hold children, which the hierarchy already provides — so it
	// costs one registration and buys the whole left column of the table above.
	//
	// @return The class id, registered on first use.
	engine::ecs::ClassId FolderClass();
}
