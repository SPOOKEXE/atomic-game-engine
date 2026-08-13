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
// | `X.luau` | a `ModuleScript` named `X` |
// | `X.server.luau` | a `Script` named `X` |
// | `X.client.luau` | a `LocalScript` named `X` |
// | `init.luau` in `D/` | `D` itself becomes a `ModuleScript`, keeping its children |
// | `init.server.luau` in `D/` | `D` itself becomes a `Script` |
// | `init.client.luau` in `D/` | `D` itself becomes a `LocalScript` |
// | `X.json` | a `ModuleScript` returning that document as a table |
// | `X.model.json` | the class, properties and children it describes |
// | `X.meta.json` | properties patched onto whatever `X` built |
// | `init.meta.json` in `D/` | the same, patched onto `D` |
// | `X.project.json` | that project, built under the node that named it |
// | `default.project.json` in `D/` | `D` **is** that project, and `D` is not also walked |
// | `X.txt` | a `StringValue` holding the file |
// | `X.csv` | a `LocalizationTable` holding the file |
//
// `.lua` is accepted everywhere `.luau` is, because Rojo's own table is written
// in terms of `.lua` and a project may predate the newer extension.
//
// **`default.project.json` replacing its folder is what makes an installed
// package arrive once.** Every wally dependency is a directory holding a project
// file whose whole tree is a `$path` — `{"tree": {"$path": "lib"}}` — beside the
// tests and examples it was published with. Following the project *and* walking
// the folder built the package twice, once under the name it publishes and once
// under the folder's own, and two copies of a `ModuleScript` are two modules
// with two states. Measured against a real game: 2012 scripts where 1082 files
// could be one, the difference being every installed package.
//
// The other half of that rule is that the nested project's **root** maps onto
// the node that included it, path and all. A build that started at the root's
// *children* built nothing for a package, because a package's root has none.
//
// `init` is the one that looks like a special case and is not: Rojo uses it so a
// script can have children, which is exactly what an instance tree is for.
// Without it a module with sub-modules would have to be a folder *beside* a
// script, and every path in the project would gain a level. **Which class the
// directory becomes is the init file's own suffix**, exactly as it is for any
// other file — reading only `init.luau` made every `init.server.luau` project a
// folder plus a stray script called `init`.
//
// ## What this does not do
//
// **Three rows of Rojo's table are still reported rather than built, and each
// names a dependency this repository does not vendor:**
//
// - **`.rbxm`** is Roblox's binary model — LZ4-framed chunks, interned strings
//   and a referent table. That is a format reader, and it belongs beside the
//   other model decoders in `bake` rather than in an editor.
// - **`.rbxmx`** is the same tree as XML, and **nothing here parses XML**.
//   `mono.vendor` holds JSON and no XML library, so this is a vendor decision
//   before it is a feature.
// - **`.toml`** would be a `ModuleScript` like `.json` is — the emitter already
//   exists and is shared — and **nothing here parses TOML** either.
//
// Each is named in the report by what Rojo says it is, so a gap reads as a gap
// rather than as an unrecognised file, and `D00104` carries what closing each
// would take.
//
// **A `.meta.json` cannot change a class.** A class is the archetype an entity
// was created in, and `Store` offers no way to move a live row between class
// trees — so `init.meta.json`'s `className` is reported as a property the
// instance does not have. Everything else in a patch is applied.
//
// **It builds, it does not watch.** A file watcher is a thread, a debounce and a
// decision about what happens when a sync lands mid-tick — and the last of those
// is rule 5's question, not a detail. Syncing on demand answers the same need
// for an author who is editing in an external editor and pressing one key to see
// it, and leaves the live-sync design to be taken deliberately.
//
// **It reads, it does not write back.** Two-way sync means conflict semantics
// for every instance the editor can touch, which `docs/retired/v07v08.md` files under
// v0.10 as a real piece of work rather than a flag.
//
// ## A universe is a folder of them
//
// One project file builds one world, because Rojo's `tree` is one place. A game
// here is a *universe* of worlds, so v0.12 adds one level above: a
// `main.universe.json` naming which world is built from which subfolder, and an
// ordinary Rojo project inside each.
//
//     main.universe.json
//     worlds/
//       main/    default.project.json   src/...
//       lobby/   default.project.json   src/...
//
// **Each world syncs on its own, and a failure is per world.** That is the whole
// reason the universe layer is a loop over independent syncs rather than one big
// build: a project file with a typo in it should cost its own world and nothing
// else. A sync that stopped at the first bad file would make one mistake look
// like the whole game was broken, and the author would have no way to tell which
// of five folders was at fault.
//
// The subfolders are ordinary Rojo projects, unchanged, so `rojo serve` and
// every other tool in that ecosystem still works on one of them in isolation.
// That is the same trade the format choice above already made.
//
// @tier client

#include <engine/ecs/Store.hpp>
#include <engine/world/Universe.hpp>

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

	// --- the universe above them ---------------------------------------------

	// One world, and where its project lives.
	//
	// @since v0.12
	struct RojoUniverseWorld {
		// The world's name. This is what `world::Universe::Find` is asked for,
		// so it is the name everything crossing a world boundary already uses —
		// a teleport, a topic, a reply.
		std::string Name;

		// The directory or project file holding it, relative to the universe
		// file.
		std::string Path;
	};

	// A parsed `main.universe.json`.
	//
	// @since v0.12
	struct RojoUniverse {
		// The universe's `name`, for a message rather than for identity.
		std::string Name;

		// The worlds, in the order they were written.
		//
		// **Order is kept rather than sorted**, for the reason `RojoNode` keeps
		// its children in order: worlds are created in this order, a world id is
		// assigned on creation, and a list that reordered itself would hand out
		// different ids for the same file.
		std::vector<RojoUniverseWorld> Worlds;
	};

	// What syncing one world did.
	//
	// @since v0.12
	struct RojoWorldSync {
		// The world named by the universe file.
		std::string World;

		// The project file that was read, once one was found.
		std::filesystem::path Project;

		// Whether the world's tree was built.
		bool Synced = false;

		// Why it was not. Empty when it was.
		//
		// **Per world rather than one error for the run**, which is the whole
		// point of the universe layer: a project file with a typo costs its own
		// world and nothing else.
		std::string Error;

		// What the sync did, when it happened.
		RojoSyncReport Report;
	};

	// What a universe sync did, world by world.
	//
	// @since v0.12
	struct RojoUniverseReport {
		// One entry per world named by the file, in that order — including the
		// ones that failed, which is what makes this readable as a result rather
		// than as a list of successes with gaps.
		std::vector<RojoWorldSync> Worlds;

		// How many worlds built.
		//
		// @return The number of entries whose `Synced` is set.
		size_t Synced() const;

		// How many did not.
		//
		// @return The number of entries whose `Synced` is clear.
		size_t Failed() const;
	};

	// Parses a universe file.
	//
	// The shape is deliberately small — a name and a map of worlds to folders —
	// because everything else about a world is already `world::WorldSettings`,
	// and a second place to say what a tick rate is would be two that disagree.
	//
	//     { "name": "MyGame", "worlds": { "Main": "worlds/main" } }
	//
	// @param json  The file's contents.
	// @param out   Filled on success.
	// @param error Filled on failure.
	// @return `false` when the document is not a universe file.
	// @since v0.12
	bool ParseRojoUniverse(std::string_view json, RojoUniverse &out, std::string &error);

	// The project file one world's entry resolves to.
	//
	// A `path` naming a file is taken as it is. A `path` naming a directory is
	// searched for **`default.project.json` first** — Rojo's own name, so a
	// subfolder is a project every tool in that ecosystem already understands —
	// and then for `main.default.json`, which `ROADMAP.md` proposed for a folder
	// that is only ever a world of this engine.
	//
	// @param root  The directory the universe file sits in.
	// @param world The world's entry.
	// @return The project file, or an empty path when there is none.
	// @since v0.12
	std::filesystem::path RojoProjectFor(const std::filesystem::path &root, const RojoUniverseWorld &world);

	// Builds every world a universe file names.
	//
	// A world already in the universe is synced into; one that is not is
	// created. Nothing is destroyed, for the reason a project sync deletes
	// nothing: a sync that removed the worlds it did not recognise would eat an
	// author's hand-built scene the first time they ran it.
	//
	// **One world's failure is that world's.** Every entry is attempted, and
	// each carries its own error — a project file that does not parse, a folder
	// with no project in it, a world the driver refused to create.
	//
	// @param universe The parsed universe file.
	// @param root     The directory it sits in. Every `path` is relative to it.
	// @param worlds   The universe to build into.
	// @param report   Filled with one entry per world, in file order.
	// @param error    Filled when nothing at all could be synced.
	// @return `false` when no world built.
	// @since v0.12
	bool SyncRojoUniverse(
		const RojoUniverse &universe,
		const std::filesystem::path &root,
		engine::world::Universe &worlds,
		RojoUniverseReport &report,
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
