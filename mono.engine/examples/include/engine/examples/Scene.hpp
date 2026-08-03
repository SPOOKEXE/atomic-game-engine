#pragma once

// The example scene, loadable by any program.
//
// The ring of orbiting, spinning cubes lived in `mono.client/src/Demo.cpp` and
// could only ever be built by a client. It is here because **a scene is not a
// client-tier idea**: a server authors the same world and replicates it, and
// the unified harness runs both halves against one. One `.luau` file, three
// programs, and the differences between them stay where they belong — a client
// adds a camera and a draw list, a server adds neither.
//
// **Two ways to move a world, and both are here on purpose.**
//
// The C++ path uses `Orbit` and `Spin` as components and iterates them in
// systems — which is what an ECS is for, and what a shipped game's hot loops
// should look like. The scripted path does not use them at all: a script
// connects to `RunService.Heartbeat` and assigns `Position` and `Orientation`
// itself, because that is how somebody writing a game actually writes one.
//
// There is deliberately **no third thing** — no component a script fills in for
// an engine system to animate. That arrangement reads like scripting and is a
// scene format wearing its clothes: the game describes, the engine decides.
// `Orbit` was that shape once and stopped being it.
//
// @tier L10 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <string>

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
	// that as `scene::WorldBounds` — measured rather than declared by the
	// script, because a scene that set its own bounds would be two sources of
	// truth for one fact and the camera frames from it.
	//
	// A failure leaves the world empty rather than half-built, so a caller can
	// stop instead of presenting something that is missing most of itself.
	//
	// @param store     The world to build into.
	// @param scheduler The systems to install.
	// @param path      The `.luau` file to run.
	// @param error     Filled in with the script's error when this returns false.
	// @return `false` when the file could not be read, compiled or run.
	bool LoadScene(ecs::Store &store, ecs::Scheduler &scheduler, const std::string &path, std::string &error);

	// The path of a scene shipped with the engine, resolved against the assets
	// root so every program finds the same file from any working directory.
	//
	// @param name The file name, such as "Rings.luau".
	// @return The absolute path.
	std::string ExamplePath(const std::string &name);
}
