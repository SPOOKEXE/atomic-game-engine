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
#include <engine/scene/DrawInstance.hpp>

#include <cstdint>
#include <vector>

namespace client {

	// --- components: per-entity, and iterated ------------------------------

	// Radians per second about each local axis.
	struct Spin {
		// Radians per second about each local axis.
		engine::core::Vector3 Rate;
	};

	// A circular path. Kept as parameters rather than integrated velocity so
	// that the scene is a pure function of the world's clock: two runs at
	// different frame rates put the cube in the same place, which is what makes
	// a frame-time comparison mean anything.
	struct Orbit {
		// The point the path goes around, in world space.
		engine::core::Vector3 Centre;

		// Distance from Centre, in metres.
		float Radius = 1.0f;

		// Angular speed. Negative runs the other way round.
		float RadiansPerSecond = 1.0f;

		// Where on the ring this entity starts, in radians.
		//
		// The only thing separating entities that share a ring: they are
		// otherwise identical, so without it the whole ring occupies one point.
		float Phase = 0.0f;

		// Offset above Centre, in metres, held for the life of the orbit. What
		// makes a set of rings a volume rather than a disc.
		float Height = 0.0f;
	};

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

	// Builds a scene of `count` orbiting, spinning cubes, registers the systems
	// that move them, and installs the resources they read. Deterministic: the
	// same count produces the same world.
	//
	// After this returns, the world is self-contained — ticking it needs the
	// store and the scheduler and nothing else.
	void BuildDemoWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t count);
}
