#pragma once

// The example scene, loadable by any program.
//
// The ring of orbiting, spinning cubes lived in `mono.client/src/Demo.cpp` and
// could only ever be built by a client. It is here because **a scene is not a
// client-tier idea**: a server authors the same world and replicates it, and
// the unified harness runs both halves against one. One `.luau` file, three
// programs, and the differences between them stay where they belong - a client
// adds a camera and a draw list, a server adds neither.
//
// The scene script builds and moves the world through the public bindings.
// The loader supplies services and a scheduler; it does not author motion.
//
// @tier L10 · shared

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine::script {
	class Runtime;
	struct RuntimeLimits;
}

namespace engine::examples {

	// Mounts a scene's own Luau modules as children of the script that uses
	// them.
	//
	// **A scene's libraries belong to the scene, not to every world in the
	// program.** These used to be mirrored into `ReplicatedStorage` from a
	// single staged `assets/lib` tree, which made them a property of the *host*:
	// `MagicCore` and `TerrainCore` appeared under every world of every game,
	// including a brand-new empty one somebody had just made, and a scene that
	// wanted them had no way to say so - it asserted that somebody else had
	// already put them there and failed when no library tree was staged. That is
	// the shape a shipped engine library has, and these are
	// a demo's modules.
	//
	// So they are staged per scene under `assets/examples/scripts/Magic` and
	// mounted under the `Script` instance itself, which is Rojo's own
	// arrangement and the one every `require(script.Parent.X)` inside the
	// modules was written against. A world that never loads `Magic.luau` has no
	// trace of any of it.
	//
	// **Idempotent by name**, for the reason a module always has: a module is
	// cached per instance, so two trees under one script would give `require`
	// two copies that share no state.
	//
	// Does nothing when the scene has no library directory, which is every scene
	// but three.
	//
	// @param store  The world the script is in.
	// @param script The `Script` or `ModuleScript` the modules belong to.
	// @param scene  The scene's file name or stem - `Magic.luau` and `Magic`
	//        both resolve to the same directory.
	// @return How many top-level modules were mounted.
	// @since v0.17
	size_t MountSceneLibraries(ecs::Store &store, ecs::Entity script, std::string_view scene);

	// Mounts a demo's matching client camera script under StarterPlayerScripts.
	// The template stays inert until a player's local script copy runs. Its
	// source is filed with the world so saved demos can move between machines.
	//
	// @param store The furnished world receiving the inert template.
	// @param builderPath The demo builder's source path.
	// @return The template, or null when no companion or container exists.
	ecs::Entity MountDemoCamera(ecs::Store &store, std::string_view builderPath);

	// Runs a scene script and schedules its heartbeat.
	//
	// Registers the scene classes first, so a script can name them,
	// then runs the file, then measures how far the result reaches and installs
	// that as `scene::WorldBounds` - measured rather than declared by the
	// script, because a scene that set its own bounds would be two sources of
	// truth for one fact and the camera frames from it.
	//
	// A failure leaves the world empty rather than half-built, so a caller can
	// stop instead of presenting something that is missing most of itself.
	//
	// **The runtime is handed back as well as kept**, and it was not until
	// v0.15. The scheduler holds the last reference and drops it with the world,
	// which is right - but a caller that needs to *reach* the VM had no way to,
	// and one does: `Runtime::DeliverGuiEvents` is how a `TextButton`'s
	// `Activated` gets from `gui::Router` to a script, and it needs the runtime
	// for the world being drawn.
	//
	// So a shipped client running a `--script` scene routed its interface input
	// correctly, produced the events correctly, and had nowhere to deliver them
	// - every button in every scripted scene was silent, in the one program a
	// game ships. `game::StartWorldScripts` already returned its runtime for the
	// same reason; this is the other loader catching up.
	//
	// Null on failure, and a caller that does not want it passes nothing.
	//
	// @param store     The world to build into.
	// @param scheduler The systems to install.
	// @param path      The `.luau` file to run.
	// @param error     Filled in with the script's error when this returns false.
	// @param runtime   Set to the VM that ran the scene, when not null.
	// @param limits    Optional host-owned limits for the scene's runtime.
	// @return `false` when the file could not be read, compiled or run.
	bool LoadScene(
		ecs::Store &store,
		ecs::Scheduler &scheduler,
		const std::string &path,
		std::string &error,
		std::shared_ptr<script::Runtime> *runtime = nullptr,
		const script::RuntimeLimits *limits = nullptr
	);

	// Compatibility helper for callers that load a script demo by name.
	// New code should use `DemosLoader` so the kind remains explicit.
	//
	// @param name The file name, such as "Rings.luau".
	// @return The absolute path.
	std::string ExamplePath(const std::string &name);

	// Compatibility helper for the shipped Luau script demos.
	// New menus should use `DemosLoader::List` so authored worlds are included.
	//
	// The loader walks the directory rather than keeping a second list of names.
	//
	// **Sorted, so the order is the same on every machine.** A directory walk is
	// in whatever order the filesystem answers in, and a menu that reshuffles
	// itself between runs is one nobody builds muscle memory for.
	//
	// Only shipped Luau builders are listed. `LoadScene` can still run a
	// caller-supplied script in another supported language.
	//
	// @return The file names, such as "Rings.luau", sorted. Empty when nothing
	//         was staged, which is a real situation rather than an error: a
	//         program built without the example target still runs.
	std::vector<std::string> ExampleScenes();
}
