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
// **Two ways to move a world, and both are here on purpose.**
//
// The C++ path uses `Orbit` and `Spin` as components and iterates them in
// systems - which is what an ECS is for, and what a shipped game's hot loops
// should look like. The scripted path does not use them at all: a script
// connects to `RunService.Heartbeat` and assigns `Position` and `Orientation`
// itself, because that is how somebody writing a game actually writes one.
//
// There is deliberately **no third thing** - no component a script fills in for
// an engine system to animate. That arrangement reads like scripting and is a
// scene format wearing its clothes: the game describes, the engine decides.
// `Orbit` was that shape once and stopped being it.
//
// @tier L10 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine::script {
	class Runtime;
}

namespace engine::examples {

	// A circular path a thing follows, in world space.
	//
	// Demo state rather than engine state, which is why it is here and not in
	// `scene`: nothing in a real game is "an orbit", and `scene` holds what a
	// thing in a world *is* rather than what one example does to it.
	//
	// @since v0.5
	struct Orbit {
		// What it circles.
		core::Vector3 Centre;

		// How far out, in metres.
		float Radius = 1.0f;

		// How fast, in radians of simulated time.
		float RadiansPerSecond = 1.0f;

		// Where on the circle it starts, in radians.
		float Phase = 0.0f;

		// How far above the centre's plane it sits.
		float Height = 0.0f;
	};

	// How fast a thing turns about its own axes, in radians per second.
	//
	// @since v0.5
	struct Spin {
		// Turn rate about local X, Y and Z.
		core::Vector3 Rate;
	};

	// Registers this module's components under explicit names.
	//
	// Idempotent. Calling it is not optional on any path that writes one: a
	// component minted under the compiler's spelling for a type is a name one
	// build can write into a recording and another cannot read back.
	// Mounts a scene's own Luau modules as children of the script that uses
	// them.
	//
	// **A scene's libraries belong to the scene, not to every world in the
	// program.** These used to be mirrored into `ReplicatedStorage` from a
	// single staged `assets/lib` tree, which made them a property of the *host*:
	// `MagicCore` and `TerrainCore` appeared under every world of every game,
	// including a brand-new empty one somebody had just made, and a scene that
	// wanted them had no way to say so - it asserted that somebody else had
	// already put them there and failed with "is assets/lib staged?" whenever
	// nobody had. That is the shape a shipped engine library has, and these are
	// a demo's modules.
	//
	// So they are staged per scene - `assets/examples/Magic/MagicCore/...` -
	// and mounted under the `Script` instance itself, which is Rojo's own
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

	// Declares the components the example scenes' own systems read.
	//
	// **Idempotent, and called by every program that runs an example.** The
	// registry replaces by name, so a client, the studio and a headless test
	// can each call it without agreeing on who goes first.
	void RegisterExampleComponents();

	// Installs the systems that move what a script built.
	//
	// Three, and every one of them iterates columns: the previous transform for
	// interpolation, the orbit, and the spin. Shared by every program, because
	// a server that simulated this scene differently from a client would be
	// disagreeing with its own replicas once per tick.
	//
	// @param scheduler The scheduler to add them to.
	void InstallMotionSystems(ecs::Scheduler &scheduler);

	// Runs a scene script and installs the systems that move it.
	//
	// Registers the components and the class first, so a script can name them,
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
	// @return `false` when the file could not be read, compiled or run.
	bool LoadScene(
		ecs::Store &store,
		ecs::Scheduler &scheduler,
		const std::string &path,
		std::string &error,
		std::shared_ptr<script::Runtime> *runtime = nullptr
	);

	// The path of a scene shipped with the engine, resolved against the assets
	// root so every program finds the same file from any working directory.
	//
	// @param name The file name, such as "Rings.luau".
	// @return The absolute path.
	std::string ExamplePath(const std::string &name);

	// Every Luau scene shipped with the engine, by file name.
	//
	// **The directory rather than a list, because a list is a second place a
	// scene has to be added to.** The studio offers these as worlds a person can
	// create, and the alternative - a table of names beside the menu - is one
	// that goes stale the first time somebody writes a scene and forgets it. The
	// staging rule is already "every `.luau` in this directory"; this reads back
	// exactly what that rule put there.
	//
	// **Sorted, so the order is the same on every machine.** A directory walk is
	// in whatever order the filesystem answers in, and a menu that reshuffles
	// itself between runs is one nobody builds muscle memory for.
	//
	// The `.ts` scenes are deliberately absent: they are staged as `.js` and
	// `LoadScene` picks its runtime off the extension, so listing the source
	// would name a file no program on this path can read. `Rings.js` and the
	// other transpiled twins are absent for the same reason - they are second
	// copies of a scene already in the list, in a second language.
	//
	// @return The file names, such as "Rings.luau", sorted. Empty when nothing
	//         was staged, which is a real situation rather than an error: a
	//         program built without the example target still runs.
	std::vector<std::string> ExampleScenes();
}
