#pragma once

// What a game file is.
//
// **A game is a universe and every world in it.** v0.7's roadmap line says the
// save file for a game contains the universe plus all subworlds, and that
// sentence is this module: `world::Universe` is the container and a world is a
// scene. A compact `.agame` carries the lot in one document. An `.auniverse`
// manifest keeps the same authored data while referring to standalone
// `.aworld` documents, so a large project can review and merge its worlds
// independently without inventing a second world schema.
//
// **Text, and specifically XML, for a reason that is not taste.**
// `world::Universe::Save` already writes a binary snapshot and keeps working;
// this is a different job. A snapshot is a running universe frozen mid-tick,
// including entity ids, tick counters and bus state - restoring one into a
// different build is not promised and never was. A game file is *authored
// content*: it has to survive an engine version, be reviewable in a diff, be
// mergeable by two people, and be repairable by hand when something goes wrong.
// Those are text's properties, and they are worth the bytes.
//
// **Which means nothing here writes an entity id.** Instances carry a
// document-local `id` attribute used only to resolve references within the same
// file, exactly as `.rbxlx` does - rule 4, arriving at a save format: a name
// crosses and a number does not.
//
// **Properties equal to their class default are not written.** A `Part` exposes
// fifteen properties and a scene sets three of them; writing all fifteen turns
// a readable file into a wall and a one-property change into a diff nobody can
// read. The consequence is stated rather than hidden: a file written by one
// build and loaded by a later one picks up that build's defaults for anything
// it did not name. That is the behaviour you want - a default that improves
// should improve existing content - and it is the same choice Roblox made.
//
// @tier L10 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Xml.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Universe.hpp>
#include <engine/world/World.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine::game {

	// Coarse preparation stages reported while a world is built off-thread.
	//
	// @since v0.22
	enum class WorldImportPhase : uint8_t {
		// Reading bytes from the selected file.
		Read,
		// Decoding and validating the XML document.
		Decode,
		// Building the standalone ECS contents.
		Build,
		// Encoding transferable snapshot bytes.
		Encode,
		// Restoring the prepared bytes into the destination universe.
		Commit,
	};

	// A world prepared away from its destination universe.
	//
	// Only settings and bytes cross the thread boundary. Committing creates the
	// destination world on its owning thread and restores this snapshot into it.
	//
	// @since v0.22
	struct PreparedWorldImport {
		// Authored world settings decoded from the document.
		world::WorldSettings Settings;
		// Portable ECS snapshot built from the document.
		std::vector<std::byte> Snapshot;
	};

	// Receives the current preparation phase and phase-local fraction.
	using WorldImportProgress = std::function<void(WorldImportPhase, float)>;

	// The format this build writes and accepts.
	//
	// A number in the file rather than a guess from its shape. A reader that
	// inferred the version from which elements were present would accept a
	// truncated file from the future as an old one.
	//
	// **2 moved a world's settings into a `<WorldProperties>` element.** They
	// were attributes on `<World>` in format 1, which put three tunables and
	// the world's identity on one line and left nowhere for a fourth to go
	// without making that line longer. `<Universe>` was already an element for
	// exactly this reason; this is the per-world half of the same shape.
	//
	// **Format 1 still reads.** The settings are looked for in the child and
	// fall back to the attributes, so a file written before this change loads
	// with its own numbers rather than with the defaults - a migration that
	// silently substituted 60 for somebody's 30 would be worse than a refusal.
	//
	// **3 adds one universe-level rendering profile library and a selected
	// profile name to each world's properties.** The graph text remains graph's
	// own format and is embedded once, so adding a second world does not copy
	// every render node.
	inline constexpr uint32_t FORMAT_VERSION = 3;

	// The extension a whole game takes.
	inline constexpr std::string_view GAME_EXTENSION = ".agame";

	// The extension a multi-file universe manifest takes.
	inline constexpr std::string_view UNIVERSE_EXTENSION = ".auniverse";

	// The extension one exported world takes.
	//
	// **A different extension from a whole game, because they are different
	// documents and confusing them is a lost afternoon.** Opening a world as a
	// game gets you a universe with no worlds; importing a game as a world gets
	// you nothing. The root element says which, so the reader refuses either
	// mistake rather than half-reading it - the extension is the courtesy.
	inline constexpr std::string_view WORLD_EXTENSION = ".aworld";

	// One remote processed-content origin declared by a universe manifest.
	// It is metadata only: callers decide whether network access is allowed.
	//
	// @since v0.21
	struct UniverseCdn {
		// User-facing source name.
		std::string Name;

		// Origin URL or location passed to the content adapter.
		std::string Location;
	};

	// Local durable storage requested by a deployed universe.
	//
	// The folder is always relative to the manifest while writing and resolved
	// beside it while loading. This keeps a package portable and prevents a
	// downloaded manifest from selecting an arbitrary host path.
	//
	// @since v0.21
	struct UniverseDataStore {
		bool Enabled = false;
		std::string Backend = "binary";
		std::filesystem::path Root = "stores";
	};

	// Controls how a multi-file universe is written.
	//
	// @since v0.21
	struct UniverseFileOptions {
		// Whether loading the manifest also discovers unlisted `.aworld` files
		// below its directory. Discovery never follows symbolic links.
		bool RecursiveWorldDiscovery = false;

		// The network capability and origins the exported universe requests.
		// Loading the document never grants this capability by itself.
		bool HttpEnabled = false;

		// Public key expected to sign content manifests from these origins.
		std::string PublisherKey;

		// Remote processed-content origins declared by the manifest.
		std::vector<UniverseCdn> Cdns;

		// Local live DataStore configuration embedded in the export.
		UniverseDataStore DataStore;
	};

	// What a game file says about itself, and the settings behind it.
	//
	// @since v0.7
	struct GameInfo {
		// The universe's name, as the studio shows it and a file remembers it.
		core::Name Name;

		// How the universe spends its workers.
		world::UniverseSettings Universe;

		// Every world the file carried, in document order.
		//
		// Names rather than handles: this is what a file said, and resolving it
		// to a `WorldId` is the caller's business after `LoadGame` has run.
		std::vector<core::Name> Worlds;

		// Named render graphs shared by every world in this universe.
		//
		// Worlds store only the profile name they select. Keeping the graph once
		// here makes an export self-contained without duplicating the same node
		// document into every world.
		graph::PipelineSet RenderingProfiles;

		// The manifest's local processed content store, resolved beside it.
		// Empty for a monolithic `.agame` or a manifest that declares none.
		std::filesystem::path Assets;

		// Capabilities and remote content locations requested by a multi-file
		// manifest. A product must ask before enabling them.
		bool HttpEnabled = false;

		// Whether unlisted worlds were requested for recursive discovery.
		bool RecursiveWorldDiscovery = false;

		// Public key expected to sign declared remote content.
		std::string PublisherKey;

		// Remote processed-content origins requested by the universe.
		std::vector<UniverseCdn> Cdns;

		// Local live DataStore configuration. Its root remains universe-relative;
		// a host resolves it against the durable deployment location rather than
		// against a temporary package extraction.
		UniverseDataStore DataStore;
	};

	// Registers every class a document can name.
	//
	// **Called by every entry point in this module rather than left to a
	// caller.** `game`'s CMakeLists names `Engine::scene` and `Engine::script`
	// for exactly this reason: a reader that depended on somebody else having
	// registered `Part` first fails with "no class named 'Part'" on a perfectly
	// good file, which sends whoever reads the message looking at the file
	// instead of at the program. A dedicated server hosting its first game file
	// found it that way.
	//
	// Idempotent and process-wide, like every other registration.
	void RegisterGameClasses();

	// --- one world ---------------------------------------------------------

	// Writes one world's instance tree and script text into an open element.
	//
	// The element is expected to be open and is not closed here - a caller
	// writing a whole game has a `<World>` open, and a caller exporting one
	// world has a `<World>` open too. One function, so the two documents cannot
	// describe a world differently.
	//
	// @param writer The document being built.
	// @param store  The world to write. Not `const`: reading properties walks
	//               the store, and `Store::Each` caches a query plan.
	void WriteWorldBody(XmlWriter &writer, ecs::Store &store);

	// Reads one world's instance tree and script text into an empty store.
	//
	// **The store must be empty.** Merging into a populated world is a
	// different operation with different answers about name collisions, and
	// pretending one function does both is how the wrong one gets called.
	//
	// @param document The parsed document, for resolving child indices.
	// @param element  The `<World>` element.
	// @param store    The world to build into.
	// @param error    Filled in with why, on failure.
	// @return `false` when the document names a class or property this build
	//         does not have, or when the store refused an instance.
	bool ReadWorldBody(
		const XmlDocument &document, const XmlElement &element, ecs::Store &store, std::string &error
	);

	// --- one instance ------------------------------------------------------

	// Writes one instance and everything under it as a standalone document.
	//
	// **What makes an instance movable between two worlds.** An `ecs::Entity`
	// is a handle inside one store and means nothing in another, so a subtree
	// cannot be handed across - it has to be described and rebuilt. This is the
	// describing half, and it is the same writer a game file uses, so an
	// instance that survives a save survives a move.
	//
	// Carries only the script text the subtree actually names, rather than the
	// whole world's: moving one part should not drag every program in the
	// source world along with it.
	//
	// @param store    The world it lives in.
	// @param instance The root of the subtree.
	// @return The document, or empty when the entity is dead or is not an
	//         instance.
	std::string WriteInstanceDocument(ecs::Store &store, ecs::Entity instance);

	// Rebuilds an instance document into a world, under a parent.
	//
	// **References out of the subtree do not survive**, and are warned about
	// rather than refused. A property pointing at something that stayed behind
	// is a handle into the world being left; leaving it at its default is what
	// a missing target already means everywhere else in this format.
	//
	// @param store    The world to build into. Need not be empty - unlike
	//                 `ReadWorldBody`, this merges, which is the whole point.
	// @param document The document, as `WriteInstanceDocument` produced it.
	// @param parent   What to parent the subtree to, or a null entity to make
	//                 it a root.
	// @param error    Filled in with why, on failure.
	// @return The rebuilt instance, or a null entity.
	ecs::Entity ReadInstanceDocument(
		ecs::Store &store, std::string_view document, ecs::Entity parent, std::string &error
	);

	// Writes one world as a standalone document, without touching a disk.
	//
	// **The half of `ExportWorld` an editor needs on its own.** Duplicating a
	// world and renaming one are both a write followed by a read - and going
	// through a temporary file to do it would make two ordinary editor actions
	// depend on somewhere being writable.
	//
	// @param universe The universe holding it.
	// @param world    Which world.
	// @param error    Filled in with why, on failure.
	// @return The document, or empty when the world is unknown or remote.
	std::string WriteWorldDocument(world::Universe &universe, world::WorldId world, std::string &error);

	// Reads a standalone world document, creating a world for it.
	//
	// @param universe The universe to create it in.
	// @param document The document, as `WriteWorldDocument` produced it.
	// @param rename   A name to use instead of the document's, or an invalid
	//                 Name to keep what the document said.
	// @param error    Filled in with why, on failure.
	// @return The new world, or an invalid handle.
	world::WorldId ReadWorldDocument(
		world::Universe &universe, std::string_view document, core::Name rename, std::string &error
	);

	// Exports one world as a standalone document.
	//
	// @param universe The universe holding it.
	// @param world    Which world.
	// @param path     Where to write.
	// @param error    Filled in with why, on failure.
	// @return `false` when the world is unknown, remote, or the file would not
	//         be written.
	bool ExportWorld(
		world::Universe &universe, world::WorldId world, const std::filesystem::path &path, std::string &error
	);

	// Imports a standalone world document, creating a world for it.
	//
	// @param universe The universe to create it in.
	// @param path     The document to read.
	// @param rename   A name to use instead of the document's, or an invalid
	//                 Name to keep what the file said. **The parameter exists
	//                 because importing the same world twice is a real thing an
	//                 author does**, and two worlds cannot share a name.
	// @param error    Filled in with why, on failure.
	// @return The new world, or an invalid handle.
	world::WorldId ImportWorld(
		world::Universe &universe, const std::filesystem::path &path, core::Name rename, std::string &error
	);

	// Reads, decodes, and builds a standalone world without touching a universe.
	bool PrepareWorldImport(
		const std::filesystem::path &path,
		PreparedWorldImport &out,
		std::string &error,
		const WorldImportProgress &progress = {}
	);

	// Adds one prepared world to its destination universe on the owning thread.
	world::WorldId CommitWorldImport(
		world::Universe &universe,
		const PreparedWorldImport &prepared,
		core::Name rename,
		std::string &error
	);

	// Starts every script a world holds, and keeps them running.
	//
	// **The one place three programs agree about what "running a game" means.**
	// The studio's Play, a dedicated server hosting a game file, and a client
	// playing one single-player all need the same four steps: open a VM over
	// the world, run the scripts the host's role selects, install the heartbeat
	// on the fixed tick delta, and keep the VM alive for as long as the world
	// is. Three copies of that would be three places for the heartbeat's delta
	// to become wall time, which is the desync rule 5 names arriving through
	// the call a script uses most.
	//
	// **The runtime is returned as a `shared_ptr` and the scheduler holds one
	// too.** A script that connects to `RunService.Heartbeat` *is* the
	// simulation for what it built, so the VM has to outlive the call that
	// started it - the same arrangement `examples::LoadScene` uses and for the
	// same reason. A caller that drops its copy leaves the world holding the
	// last one.
	//
	// **Luau, and a world whose scripts are JavaScript runs nothing.** The
	// runtime is per world and `script::LanguageOf` picks per file;
	// reconciling those is a design decision rather than an oversight, and it
	// is stated here rather than discovered.
	//
	// @param store     The world.
	// @param scheduler The systems to install the heartbeat into.
	// @param limits    What bounds a script, including the host's role.
	// @param error     Filled in with the first script failure, if any. A
	//                  failure does not stop the others - a world where half
	//                  the scripts silently did not start is a bug report with
	//                  nothing in it.
	// @param breakpoints Optional. Adopted by the runtime **before** its scripts
	//                    run, which is the only ordering that lets a breakpoint
	//                    on a script's top level fire at all - that code has
	//                    already executed by the time this returns.
	// @return The runtime, which is never null.
	std::shared_ptr<script::Runtime> StartWorldScripts(
		ecs::Store &store,
		ecs::Scheduler &scheduler,
		const script::RuntimeLimits &limits,
		std::string &error,
		const script::Debugger *breakpoints = nullptr
	);

	// --- the whole game ----------------------------------------------------

	// Writes a universe and every local world in it.
	//
	// **A remote world is written as a name and a setting, not as content.** A
	// world held by a supervised host has no store here, so its instances are
	// not this process's to save - and writing an empty one would be a save
	// file that quietly deleted somebody's scene.
	//
	// @param universe The universe to write.
	// @param name     The game's name.
	// @param path     Where to write.
	// @param error    Filled in with why, on failure.
	// @return `false` when the file would not be written.
	bool SaveGame(
		world::Universe &universe, core::Name name, const std::filesystem::path &path, std::string &error
	);

	// Writes a universe with its shared rendering profile library.
	bool SaveGame(
		world::Universe &universe,
		core::Name name,
		const graph::PipelineSet &renderingProfiles,
		const std::filesystem::path &path,
		std::string &error
	);

	// Writes a manifest and one standalone document per local authored world.
	//
	// World files are written below a sibling `worlds/` directory and named in
	// the manifest. The manifest is written last, so a failed world write never
	// publishes references to incomplete content.
	//
	// @param universe         The universe to write.
	// @param name             The universe's authored name.
	// @param renderingProfiles Its shared rendering profile library.
	// @param path             The `.auniverse` manifest path.
	// @param options          Discovery behavior recorded in the manifest.
	// @param error            Filled in with why, on failure.
	// @return `false` when any world or the manifest could not be written.
	bool SaveUniverse(
		world::Universe &universe,
		core::Name name,
		const graph::PipelineSet &renderingProfiles,
		const std::filesystem::path &path,
		const UniverseFileOptions &options,
		std::string &error
	);

	// Replaces a universe from a multi-file manifest.
	//
	// Relative world references cannot leave the manifest directory. Recursive
	// discovery is deterministic, bounded, and skips file and directory links.
	//
	// @param universe The universe to replace.
	// @param path     The `.auniverse` manifest to read.
	// @param out      Filled in with its name, settings, profiles, and worlds.
	// @param error    Filled in with why, on failure.
	// @return `false` when the manifest or any referenced world is invalid. A
	//         manifest rejected before construction leaves the existing universe
	//         untouched; failure after construction begins leaves it empty.
	bool LoadUniverse(
		world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error
	);

	// Adds a game file's worlds to a universe, keeping what is already there.
	//
	// **The merging counterpart to `LoadGame`, and the reason both exist.**
	// Loading replaces: it empties the universe first, because one that is half
	// of one game and half of another is `ecs::Store::Load`'s hazard a layer up.
	// Importing is the other operation - bringing a colleague's scenes into the
	// game already open - and it is what `ImportWorld` does for one world.
	//
	// A world whose name is taken is renamed with a numeric suffix rather than
	// refused; two worlds cannot share a name and being made to guess a free
	// one is the worse answer.
	//
	// **A failure part-way keeps what already read.** The worlds imported
	// before the bad one are good, and discarding them because the fourth scene
	// names a missing class would throw away work that loaded perfectly.
	//
	// @param universe The universe to add to.
	// @param path     The `.agame` or `.auniverse` to read.
	// @param out      Filled in with what the file said, and the names the
	//                 worlds were actually created under.
	// @param error    Filled in with why, when fewer worlds arrived than the
	//                 file held.
	// @return How many worlds were created.
	size_t ImportUniverse(
		world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error
	);

	// Replaces a universe's worlds with a game file's.
	//
	// **Every existing world is destroyed first**, for `ecs::Store::Load`'s
	// reason: a universe that is partly one game and partly another looks like
	// it works, right up until two scripts named the same thing disagree about
	// which world they are in.
	//
	// @param universe The universe to build into.
	// @param path     The document to read.
	// @param out      Filled in with what the file said.
	// @param error    Filled in with why, on failure.
	// @return `false` when the file would not be read or parsed. On failure the
	//         universe is left **empty** rather than half loaded.
	bool
	LoadGame(world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error);

	// Builds the document a `SaveGame` would write, without touching a disk.
	//
	// For a test, and for a studio comparing what is on screen against what was
	// last saved - which is how the title bar knows to show a modified marker
	// without keeping a second copy of the world to diff against.
	//
	// @param universe The universe to write.
	// @param name     The game's name.
	// @return The document.
	std::string WriteGame(world::Universe &universe, core::Name name);

	// Builds a game document carrying the universe's rendering profiles.
	std::string
	WriteGame(world::Universe &universe, core::Name name, const graph::PipelineSet &renderingProfiles);
}
