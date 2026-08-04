#pragma once

// The v0.1 demo scene.
//
// **The components are `mono.engine/scene`'s and nothing here declares one.**
// This file used to carry a `Transform`, a `PreviousTransform`, a `Visual`, a
// `SceneBounds` and an `ActiveCamera` of its own, because the ECS is storage
// and does not know what a Transform is and there was nowhere shared to put
// them. `scene` at L7 is that place, both programs register the same set under
// the same names, and a snapshot now crosses between them with no translation
// layer. What is left here is the demo: `Spin` and `Orbit`, which describe how
// this scene moves and nothing else does, and `DrawList`, which is what one
// world hands its compositor.
//
// **There is no scene object.** Building the world is a function, and
// everything the tick touches is in the store: per-entity data as components,
// world-scoped data as resources. That is not tidiness — a scene class with the
// draw list and the clock as members puts the state the renderer reads outside
// the world, where the affinity check does not cover it, the profiler does not
// see it, and a second world cannot have its own.

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstdint>
#include <vector>

namespace client {

	// --- components: per-entity, and iterated ------------------------------
	//
	// `Orbit` and `Spin` used to be declared here, and they moved to
	// `engine::examples` at v0.5 for the reason that module's CMakeLists gives:
	// a scene is not a client-tier idea. A server authors the same world and
	// replicates it, so a component only a client could name was a component
	// only a client could ever build a scene out of. These are the same two
	// types under `examples.Orbit` and `examples.Spin`.

	using engine::examples::Orbit;
	using engine::examples::Spin;

	// --- resources: one of each, for the whole world -----------------------

	// What to draw this frame, as the *world* describes it.
	//
	// In the world rather than beside it, because the alternative is the thing
	// repo_layout.md §1 names outright: a module keeping a private vector for
	// data another module reads. The vector's capacity survives from frame to
	// frame, so a steady scene stops allocating after the first one.
	struct DrawList {
		// One entry per visible cube, rebuilt every frame.
		//
		// `scene::DrawInstance`, not a renderer's instance: a `server`-tier host
		// publishes one of these too, so the payload cannot be a type only a
		// client can name. The conversion into a matrix and an RGBA happens in
		// `render`, once, at the point of upload.
		//
		// Cleared rather than reallocated, so the capacity survives from frame
		// to frame and a steady scene stops allocating after the first one.
		std::vector<engine::scene::DrawInstance> Instances;
	};

	// The world's first surface camera, if it has one.
	//
	// **First rather than all**, and this pipeline renders one offscreen view.
	// A world with two surface cameras gets the first by entity id — which is
	// creation order — and the second is ignored. Said here rather than left to
	// be discovered: the render-node system that replaces this pipeline is where
	// several belong, and a silent pick would make its absence look like a bug
	// in the mirror rather than a limit of the pipeline.
	//
	// @param store   The world to search.
	// @param surface Filled in when one is found.
	// @return `false` when the world has no surface camera.
	bool FindSurfaceCamera(engine::ecs::Store &store, engine::render::SurfaceView &surface);

	// Builds the scene by running a Luau file instead of a C++ loop.
	//
	// The entities, the components and the systems that move them all come from
	// `engine::examples::LoadScene`, which every program shares. What this adds
	// is the client's half and only that: a camera to look through and a draw
	// list to fill. A server calls the same loader and adds neither.
	//
	// @param store     The world to build into.
	// @param scheduler The systems to install.
	// @param path      The `.luau` file to run.
	// @param reserve   How much draw-list capacity to reserve up front.
	// @return `false` when the script could not be read, compiled or run.
	bool BuildScriptedWorld(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &scheduler,
		const std::string &path,
		uint32_t reserve
	);

	// The client's half of a world, installed onto one somebody else built.
	//
	// **`BuildScriptedWorld` is the demo's entry point and this is the general
	// one.** A studio opens a world out of a game file rather than out of a
	// `.luau`, and a world with no draw list is a world that renders as an
	// empty frame — which reads as a broken renderer rather than as a missing
	// system, and cost an afternoon to find once.
	//
	// Installs a `DrawList`, the previous-transform capture that rendering
	// interpolates from, and the collector that fills the list. Does **not**
	// install `move-camera`: a world being edited is looked at through the
	// editor's camera, and a second thing writing the same `Transform` is the
	// bug `BuildScriptedWorld`'s comment describes.
	//
	// @param store     The world.
	// @param scheduler The systems to install into.
	// @param reserve   How much draw-list capacity to reserve up front.
	void
	InstallPresentation(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t reserve = 0);

	// Gives a world an orbiting camera, when it has none of its own.
	//
	// **A scene that placed its own camera keeps it**, which is the rule
	// `BuildScriptedWorld` already holds and the reason it is a rule: running
	// the placeholder orbit beside a script that aimed a camera is two things
	// writing one `Transform`, the second winning silently every tick. That is
	// not hypothetical — it made `Mirrors-1-world.luau` compute its reflection
	// for a position the viewer was no longer at, and the mirror looked broken.
	//
	// A game file's world usually has no camera, because a camera is something
	// a script makes and the studio does not author one. So single-player needs
	// this and the editor does not: the editor looks through its own camera,
	// which is not an entity in any world.
	//
	// @param store     The world.
	// @param scheduler The systems to install into.
	// @return `true` when a camera was installed, `false` when the world
	//         already had one.
	bool InstallDefaultCamera(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);

	// Registers this module's own types under explicit names.
	//
	// **One type, and it had no registration at all until v0.7.** `DrawList` is
	// a resource, a resource is keyed by a component id, and
	// `Store::SetResource` was minting one under the compiler's spelling of the
	// type — which is rule 4's exact failure and sat unnoticed because nothing
	// had ever snapshotted a world that had one. The studio's Stop does.
	//
	// Idempotent, and called by both entry points above. Call it before
	// anything touches a `DrawList`: `Components::Of<T>` caches its answer per
	// type per process, so an explicit registration arriving second aborts
	// rather than leaving two names for one thing.
	void RegisterClientComponents();
}
