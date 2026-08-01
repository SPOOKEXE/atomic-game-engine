#pragma once

// The v0.1 demo scene.
//
// Components and resources live here rather than in `ecs` because the ECS is
// storage and does not know what a Transform is, and rather than in `render`
// because a transform is not a presentation concept. They move to `scene` at L7
// when v0.3 brings the real Basic Components set; until then the program that
// uses them owns them, which is the smallest place they can live.
//
// **There is no scene object.** Building the world is a function, and
// everything the tick touches is in the store: per-entity data as components,
// world-scoped data as resources. That is not tidiness — a scene class with the
// draw list and the clock as members puts the state the renderer reads outside
// the world, where the affinity check does not cover it, the profiler does not
// see it, and a second world cannot have its own.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>

#include <cstdint>
#include <vector>

namespace client {

	// --- components: per-entity, and iterated ------------------------------

	// Where a thing is. One CFrame, no scale — scale belongs to what is being
	// drawn, not to where it is.
	struct Transform {
		engine::core::CFrame Frame;
	};

	// Where it was at the start of the current tick.
	//
	// Rendering runs faster than the simulation ticks, so drawing at tick
	// positions makes everything judder at the beat frequency between the two.
	// The render interpolates from here to Transform by the clock's alpha.
	//
	// Deliberately its own component rather than reusing a previous-frame
	// matrix kept for velocity or temporal AA. RENDER_PIPELINE.md §14 flags
	// that reuse as coupling two features that are independent, and it is
	// cheaper to keep them apart now than to untangle them later.
	struct PreviousTransform {
		engine::core::CFrame Frame;
	};

	// Radians per second about each local axis.
	struct Spin {
		engine::core::Vector3 Rate;
	};

	// A circular path. Kept as parameters rather than integrated velocity so
	// that the scene is a pure function of the world's clock: two runs at
	// different frame rates put the cube in the same place, which is what makes
	// a frame-time comparison mean anything.
	struct Orbit {
		engine::core::Vector3 Centre;
		float Radius = 1.0f;
		float RadiansPerSecond = 1.0f;
		float Phase = 0.0f;
		float Height = 0.0f;
	};

	struct Visual {
		engine::core::Color3 Colour;
		float Size = 1.0f;
	};

	// --- resources: one of each, for the whole world -----------------------

	// How far the scene reaches from the origin. Written once while building,
	// read by the camera system.
	struct SceneBounds {
		float Extent = 1.0f;
	};

	// Where the world is looked at from.
	//
	// A resource rather than a component on a camera entity: there is one, and
	// nothing iterates it. Componentising it would buy an archetype, a query
	// and a loop that runs once, and would turn "where is the camera" from a
	// lookup into a search. GARG_ECS_Layout.md §5.
	struct ActiveCamera {
		engine::render::Camera Value;
	};

	// What to draw this frame, in the flat form the renderer wants.
	//
	// In the world rather than beside it, because the alternative is the thing
	// repo_layout.md §1 names outright: a module keeping a private vector for
	// data another module reads. The vector's capacity survives from frame to
	// frame, so a steady scene stops allocating after the first one.
	struct DrawList {
		std::vector<engine::render::Instance> Instances;
	};

	// Builds a scene of `count` orbiting, spinning cubes, registers the systems
	// that move them, and installs the resources they read. Deterministic: the
	// same count produces the same world.
	//
	// After this returns, the world is self-contained — ticking it needs the
	// store and the scheduler and nothing else.
	void BuildDemoWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t count);
}
