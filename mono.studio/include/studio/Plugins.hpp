#pragma once

// Scripts the editor runs, written by somebody who is not the editor.
//
// **A plugin is a script against the world, and that is the whole design.** The
// engine already has a scripting surface with a sandbox, a step budget, a memory
// ceiling and two languages — `script/Runtime.hpp` — and a plugin is that surface
// pointed at the world an author is editing rather than at one a game is running.
// Inventing a second scripting model for tools would be two sandboxes to keep
// safe and two vocabularies to learn.
//
//     ~/Documents/atomic-game-engine/studio/plugins/
//       align-tool/
//         plugin.json     { "name": "Align", "main": "main.luau" }
//         main.luau
//
// ## What a plugin can reach, and why it needs no new surface for it
//
// Everything a game script can: `Instance`, `workspace`, `game`, the datatypes,
// and — since v0.12 — `World`, the ECS underneath. That last one is what makes a
// tool possible without an editor API: a plugin declares a component, queries
// for entities carrying one, and writes values back, all through the same
// storage the editor is looking at.
//
// **The selection is a component, not a function call.** `studio.Selected` is a
// described component the editor puts on whatever is selected and takes off
// whatever is not, so a plugin reads it with `World:Query("studio.Selected")`
// and changes it with `entity:SetComponent("studio.Selected", {})`. The editor
// reads it back the same frame.
//
// That is deliberate rather than a shortcut around a missing API. A selection
// *is* per-entity state about the world, which is what a component is for — and
// putting it in the store means a plugin, a C++ system and the properties panel
// are three readers of one fact rather than three copies of it. `ecs/AGENTS.md`
// rule 2 is the argument, and it applies to the editor's own state as much as to
// a game's.
//
// ## What a plugin cannot reach, stated rather than discovered
//
// **There is no toolbar API and no editor command surface.** A plugin runs on
// `RunService.Heartbeat` and acts on the world; it cannot add a button, open a
// panel or invoke a menu item. Those need a channel between a script and the
// editor that is neither the world nor a `lua_State` crossing a module
// boundary — `script/AGENTS.md` forbids the second — and designing that channel
// is a piece of work rather than a parameter. `D00105` carries what it would
// take.
//
// ## One runtime each, and one failure each
//
// **Every plugin gets its own `script::Runtime`.** Two plugins sharing one would
// share a global table, a step budget and a memory ceiling, so a plugin that
// looped would stop the others and a plugin that set a global would be read by
// them. The cost is a VM per plugin, which is the same trade the engine already
// makes per world.
//
// **A plugin that fails is switched off and named.** It does not stop the load,
// it does not stop the beat, and the reason is kept where somebody can read it —
// the same rule the universe sync follows one file over, for the same reason: an
// author with five plugins and one mistake has to be told which.
//
// @tier client

#include <engine/ecs/Store.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Universe.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace studio {

	// The component the editor's selection is published as.
	//
	// **A tag — no fields — because the fact is binary.** Whether an instance is
	// selected is the whole of it; anything else about the selection is a
	// property of the *editor*, not of the instance, and belongs where the rest
	// of the editor's state does.
	//
	// @since v0.12
	inline constexpr std::string_view SELECTED_COMPONENT = "studio.Selected";

	// Registers `studio.Selected`, so a plugin can name it before anything is
	// selected.
	//
	// **Idempotent, and called when the editor starts rather than when the first
	// thing is selected.** A component registered mid-session takes an id
	// decided by how long somebody browsed before clicking, and `ecs::Components`
	// exists to stop exactly that.
	//
	// @return `true` when the component is registered and usable.
	// @since v0.12
	bool RegisterSelectionComponent();

	// What a `plugin.json` says.
	//
	// @since v0.12
	struct PluginManifest {
		// What it calls itself, for a list and a log line.
		std::string Name;

		// One sentence, shown beside the name.
		std::string Description;

		// The entry script, relative to the plugin's own folder.
		//
		// **Relative and refused if it escapes**, which is the one piece of
		// path handling here that is a decision rather than plumbing: a `main`
		// of `../../../../etc/passwd` is a plugin reading a file outside its
		// folder, and a manifest is a file somebody downloaded.
		std::string Main = "main.luau";

		// Whether the editor should run it. A plugin somebody switched off stays
		// on disk and stays listed.
		bool Enabled = true;
	};

	// One plugin, as the editor holds it.
	//
	// @since v0.12
	struct LoadedPlugin {
		// Its folder, which is also its identity: two plugins may call
		// themselves the same thing and they are still two plugins.
		std::filesystem::path Root;

		// What its manifest said.
		PluginManifest Manifest;

		// Its own VM, or null when it could not be started.
		std::shared_ptr<engine::script::Runtime> Vm;

		// Whether it is running. False for one that was switched off, and for
		// one that failed.
		bool Running = false;

		// Why it is not, in the words somebody can act on. Empty when it is.
		std::string Error;

		// How many times its heartbeat has raised. A plugin that throws every
		// frame is switched off rather than logged sixty times a second.
		size_t Faults = 0;
	};

	// How many times a plugin may raise before it is switched off.
	//
	// **Small, because the failure is per frame.** A plugin whose heartbeat
	// throws does it again next frame and every frame after; three is enough to
	// tell a transient from a broken one and few enough that the log stays
	// readable.
	inline constexpr size_t PLUGIN_FAULT_LIMIT = 3;

	// Where plugins are looked for.
	//
	// `ConfigRoot() / "plugins"`, so it moves with the rest of the studio's
	// configuration and a test can point it somewhere else in one call.
	//
	// @return The directory, which may not exist.
	// @since v0.12
	std::filesystem::path PluginRoot();

	// Parses a `plugin.json`.
	//
	// @param json  The file's contents.
	// @param out   Filled on success.
	// @param error Filled on failure.
	// @return `false` when the document is not a plugin manifest.
	// @since v0.12
	bool ParsePluginManifest(std::string_view json, PluginManifest &out, std::string &error);

	// Every plugin folder under `PluginRoot`, in name order.
	//
	// **Sorted, because a directory walk is not ordered.** Plugins run in this
	// order and one may build on what another left in the world, so an order
	// that changed between sessions would be a scene that came up differently
	// depending on the filesystem.
	//
	// A folder with no manifest is skipped in silence — it is somebody's notes,
	// not a broken plugin. One with a manifest that does not parse is returned
	// with its `Error` set, because that one *is* broken and saying so is the
	// point.
	//
	// @param root Where to look.
	// @return One entry per plugin folder, ordered by folder name.
	// @since v0.12
	std::vector<LoadedPlugin> DiscoverPlugins(const std::filesystem::path &root);

	// Starts every discovered plugin against a world.
	//
	// **One `script::Runtime` each**, so a plugin cannot see another's globals
	// or spend another's step budget. A plugin whose entry script fails to run is
	// left with `Running` clear and its error kept; the rest still start.
	//
	// @param plugins What `DiscoverPlugins` found, updated in place.
	// @param store   The world they run against.
	// @since v0.12
	void StartPlugins(std::vector<LoadedPlugin> &plugins, engine::ecs::Store &store);

	// Beats every running plugin once.
	//
	// **One frame's delta, and a plugin that raises is counted rather than
	// stopped.** Past `PLUGIN_FAULT_LIMIT` it is switched off with its last
	// error kept — a plugin throwing sixty times a second is a log nobody can
	// read and a frame nobody can profile.
	//
	// @param plugins What `StartPlugins` started.
	// @param delta   Seconds since the last beat.
	// @return How many plugins were beaten.
	// @since v0.12
	size_t BeatPlugins(std::vector<LoadedPlugin> &plugins, float delta);
}
