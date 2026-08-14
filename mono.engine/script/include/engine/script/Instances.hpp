#pragma once

// A script is an instance, and a world owns its scripts.
//
// **This is what makes "scripts live under a world" structural rather than a
// fact about the command line.** `--script PATH` runs one file against a world;
// a game has many, each parented somewhere, each knowing which one it is. Until
// a script is a row in the world it builds, a world cannot be written out whole
// — so this is the prerequisite for a save file rather than a convenience.
//
// **The source is a path, not the text**, and that is a v0.6 decision worth
// stating. Roblox's `Script.Source` is the program itself, and a save format
// that carried it would need a component holding an unbounded string — which is
// not trivially copyable, so it could not be a column, could not go into a
// snapshot as its object representation, and could not cross a bus as bytes. A
// `core::Name` naming an asset-relative path is what `Visual::Mesh` already is
// for the same reason, and it is what a save file can carry today. When v0.7's
// game file arrives with a place to put script text, this is the field that
// changes.
//
// **Which scripts run is the host's role, not the world's.** A `Script` runs
// where `RunService:IsServer()` is true and a `LocalScript` where `IsClient()`
// is — Roblox's rule, and the one an author already expects. A single-player
// host is both, so it runs both, which is exactly right.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Language.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace engine::script {

	class Runtime;

	// Where a script's Luau program is read from.
	//
	// **One container per language, and an instance may hold both.** A script
	// being ported does not stop being one script: the Luau it has and the
	// JavaScript it is becoming are two programs of one instance, and a single
	// `Source` field made switching between them a destructive edit — the old
	// text was overwritten by the new one, and going back meant having kept a
	// copy somewhere the engine knew nothing about.
	//
	// **The path, not the text.** `SourceCache` holds text keyed by path and is
	// a world resource, for the reason it gives: a component must be trivially
	// copyable to be a column, so it cannot hold an unbounded string.
	//
	// **Not scriptable, and that is the security boundary rather than a
	// preference.** A script that could write another script's source is a
	// sandbox escape that makes every other boundary in this engine decorative
	// — the step budget, the memory ceiling and the host role all assume the
	// program is the one an author wrote. The properties panel, a game file and
	// the Rojo sync all write it, because they are the author.
	// `ecs::PropertyDescriptor::Scriptable` carries the mechanism.
	//
	// @since v0.14
	struct LuaSourceContainer {
		// The path, relative to the assets root. An invalid name is a container
		// with nothing in it, which is a legal state — an author makes the
		// instance before choosing the file.
		core::Name Path;
	};

	// Where a script's JavaScript program is read from.
	//
	// The twin of `LuaSourceContainer`, and deliberately a second component
	// rather than a second field on one: an archetype is what a query walks, so
	// a world of Luau scripts pays nothing for the JavaScript column it does
	// not have.
	//
	// @since v0.14
	struct JavaScriptSourceContainer {
		// The path, relative to the assets root.
		core::Name Path;
	};

	// Which of an instance's containers is the program it runs.
	//
	// **The one part of this a script may set.** Choosing which language a
	// script runs is a decision a game can legitimately make at run time — a
	// mod switching an implementation, a test running the same behaviour twice
	// — and none of it requires reading or writing a line of anybody's source.
	// So this is scriptable and the two containers are not, which is the whole
	// point of splitting the selection out of them.
	//
	// **Absent means Luau**, which is what every script in this engine was
	// before there were two, so a world loaded from an older file runs exactly
	// as it did.
	//
	// @since v0.14
	struct CodeSourceContainerSelector {
		// Which container `ActiveSourceOf` answers with.
		Language Active = Language::Luau;
	};

	// The path of whichever container the selector points at.
	//
	// **One rule, asked by everybody.** Both runtimes, the debugger, the script
	// editor and the game file all need "what does this instance run", and four
	// answers to that is three chances to run the wrong program.
	//
	// @param store    The world.
	// @param instance The script instance.
	// @return The path, or an invalid name when the selected container is empty
	//         or absent.
	// @since v0.14
	core::Name ActiveSourceOf(const ecs::Store &store, ecs::Entity instance);

	// Which language an instance is set to run.
	//
	// @param store    The world.
	// @param instance The script instance.
	// @return The selector's language, or `Language::Luau` when there is none.
	// @since v0.14
	Language ActiveLanguageOf(const ecs::Store &store, ecs::Entity instance);

	// Points an instance at a path, filling the container the extension names.
	//
	// **The extension decides which container, and the selector follows it.**
	// That is what makes `--script thing.js` and a Rojo `.luau` file both do the
	// obvious thing with one call, and it is where `LanguageOf` is applied — a
	// caller choosing the container itself would be a second place that decides
	// what a `.ts` file is.
	//
	// @param store    The world.
	// @param instance The script instance.
	// @param path     The path to run.
	// @since v0.14
	void SetSourcePath(ecs::Store &store, ecs::Entity instance, core::Name path);

	// A script the host must not run.
	//
	// **A tag rather than a bool**, so a disabled script is in a different
	// archetype and the run loop never visits it. `Anchored` makes the same
	// trade in `scene`, for the same reason: presence is a query, a flag is a
	// branch on every row.
	//
	// @since v0.6
	struct Disabled {};

	// Registers `Script`, `LocalScript` and their components.
	//
	// Process-wide and idempotent, like every other class registration.
	//
	// `ModuleScript` is registered here too, and it is a **sibling** of `Script`
	// rather than a kind of one — which is what makes it inert. `ScriptsIn`
	// collects `IsA(Script)` and `IsA(LocalScript)`; a module is neither, so the
	// run loop never visits one and nothing had to learn to skip it.
	//
	// @return The `Script` class id.
	ecs::ClassId ScriptClass();

	// The `LocalScript` class id, registering the tree on first call.
	//
	// @return The class id.
	ecs::ClassId LocalScriptClass();

	// The `ModuleScript` class id, registering the tree on first call.
	//
	// **Nothing runs a module.** It is reached with `require`, which evaluates it
	// once per runtime and hands every later caller the same value back. A module
	// with a side effect at its top level therefore has that side effect once, on
	// whichever script required it first — Roblox's rule, and the reason module
	// order is not something an author has to think about.
	//
	// @return The class id.
	ecs::ClassId ModuleScriptClass();

	// Every script instance a host of this role should run, in a stable order.
	//
	// **Ordered by entity id, which is creation order**, so a world loaded the
	// same way twice runs its scripts in the same sequence. That matters more
	// than it looks: one script may build what another expects to find, and an
	// order that depended on archetype layout would reorder itself the first
	// time an unrelated component was added.
	//
	// **Takes the store mutably, because asking it a question is a mutation.**
	// A query is built and cached on first use, so `Store::Each` is non-const by
	// design — and a `const Store &` here bought nothing except a `const_cast`
	// at the one line that had to do the work. Naming the requirement in the
	// signature is the honest version: this reads no rows the caller wrote, but
	// it is not a call you may make from a thread that does not own the world.
	//
	// @param store  The world.
	// @param server Whether this host simulates authoritatively.
	// @param client Whether this host presents.
	// @return The instances to run, in order.
	std::vector<ecs::Entity> ScriptsIn(ecs::Store &store, bool server, bool client);

	// Every script instance a client should run in a world it does not own.
	//
	// **A class rule and a container rule, because a replica needs both.**
	// `ScriptsIn` answers the class half — a `Script` is the server's and a
	// `LocalScript` is a client's — and that is the whole answer for a host that
	// owns the world it is running. It is not the answer for a replica: the rows
	// there are somebody else's, and a client that ran every `LocalScript` it
	// could see would run the ones in *other people's* players and the ones in
	// `StarterPlayerScripts`, which is a template rather than a program.
	//
	// **Roblox's containers, and no new vocabulary was needed for them.** A
	// `LocalScript` runs when it is under the local player's own subtree —
	// `scene::PlayerOwning` against `scene::LocalPlayer` — or under
	// `ReplicatedFirst`, which is `scene::InReplicatedFirst`. `scene::AddPlayer`
	// is what copies `StarterPlayerScripts` into a player's `PlayerScripts`, so
	// the template's own children are excluded by being where they are rather
	// than by being named here.
	//
	// **This is not `ScriptsIn` with an extra argument, and that is deliberate.**
	// A single-player host is a server *and* a client and owns the world it is
	// in; the containment rule there would stop a `LocalScript` an author parked
	// in `Workspace` from ever running, which is a change to how every existing
	// scene loads for the sake of a rule about a replica.
	//
	// @param store The replicated world.
	// @return The instances to run, in creation order. Empty until the host has
	//         said which player is this client's.
	// @since v0.15
	std::vector<ecs::Entity> ClientScriptsIn(ecs::Store &store);

	// Creates a script instance naming a file.
	//
	// @param store The world.
	// @param path  The asset-relative path to read the program from.
	// @param name  The instance's name.
	// @param local Whether this is a `LocalScript` rather than a `Script`.
	// @return The instance, or `NULL_ENTITY` when the world refused it.
	ecs::Entity
	MakeScript(ecs::Store &store, std::string_view path, std::string_view name, bool local = false);

	// Creates a `ModuleScript` naming a file.
	//
	// @param store The world.
	// @param path  The asset-relative path to read the program from.
	// @param name  The instance's name, which is what `require` reaches it by.
	// @return The instance, or `NULL_ENTITY` when the world refused it.
	ecs::Entity MakeModule(ecs::Store &store, std::string_view path, std::string_view name);

	// Mirrors a directory of `.luau` files into a tree of `ModuleScript`s.
	//
	// **This is what makes a library of many files reachable from a script.**
	// `require` takes an instance and never a path, deliberately — so a
	// thousand-file library has to *be* a thousand instances before anything can
	// require the first one, and until this existed the only way to get one was
	// to hand-build the tree in C++ or to write the whole library into one file.
	//
	// **Rojo's layout rule, because that is the one authors already know.** A
	// directory becomes a plain `Instance` acting as a folder; a `.luau` file
	// becomes a `ModuleScript` named after its stem; and an `init.luau` collapses
	// into its own directory, so `Presets/init.luau` *is* the `Presets` module
	// and its siblings become its children. Getting that last rule wrong is
	// silent: `script.Parent.X` resolves one level off and every module in the
	// library fails at its first require.
	//
	// **Absolute paths, and that is not incidental.** `ReadSource` resolves a
	// relative `Source` against `core::Paths::Assets()`, which defaults to the
	// running program's own directory — while a staged library sits in a
	// *sibling* of it. `examples::ExamplePath` documents that mismatch and works
	// around it by looking in both places; a `Source` cannot, because it is one
	// name. Resolving here, once, where the directory is already known, keeps
	// the layout out of every file that gets mounted.
	//
	// Entries that are neither a directory nor a `.luau` file are skipped, and a
	// directory holding nothing to mount produces no instance rather than an
	// empty one.
	//
	// @param store     The world.
	// @param directory The directory to mirror. Read once, at call time.
	// @param name      What the root instance is called.
	// @param parent    What to parent the root to, or `NULL_ENTITY` for a root.
	// @return The root instance, or `NULL_ENTITY` when the directory held
	//         nothing or the world refused an instance.
	ecs::Entity MountModuleTree(
		ecs::Store &store,
		const std::filesystem::path &directory,
		std::string_view name,
		ecs::Entity parent = ecs::NULL_ENTITY
	);
}
