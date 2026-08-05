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

#include <string>
#include <vector>

namespace engine::script {

	class Runtime;

	// Where a script's program is read from.
	//
	// @since v0.6
	struct Source {
		// The path, relative to the assets root. An invalid name is a script
		// with nothing to run, which is a legal state — an author makes the
		// instance before choosing the file.
		core::Name Path;
	};

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
}
