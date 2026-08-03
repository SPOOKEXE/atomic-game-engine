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

	bool BuildScriptedWorld(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &scheduler,
		const std::string &path,
		uint32_t reserve
	);
}
