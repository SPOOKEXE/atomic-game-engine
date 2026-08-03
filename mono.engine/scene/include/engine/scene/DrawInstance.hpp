#pragma once

// One drawable thing, as the *world* describes it.
//
// This is the payload a world publishes on `world::ViewChannel`, and it
// replaces `render::Instance` as the thing that crosses from simulation to
// presentation. The two are not the same type wearing different names:
//
// | | `scene::DrawInstance` | `render::Instance` |
// |---|---|---|
// | Says | a cube of oak, here | a `mat4` and an RGBA |
// | Tier | `shared` | `client` |
// | Written by | whoever ticks the world | the renderer's own upload |
//
// **It has to be `shared`, and that is the whole reason it is here.** A
// server-tier host produces one — a headless world still has a draw list, and
// `world::ViewChannel` is how a hosted world's view reaches a client — while a
// client-tier consumer reads it. A type only one of those tiers can name cannot
// be the thing they hand between them.
//
// So it carries **scene data, never device data**: a `CFrame` rather than a
// column-major `mat4`, a `core::Name` rather than a mesh handle, a `Color3`
// rather than a packed RGBA. The conversion to whatever a GPU wants belongs in
// the presentation module, once, at the point of upload — putting it here would
// put a device's memory layout in the type a headless server writes.
//
// `HalfExtent` is the one field `v02v03v04.md` does not name and it is not
// optional: a `CFrame` carries no scale on purpose, and the `mat4` this
// replaces carried it. Without it a two-metre cube and a one-metre cube are the
// same draw instance, and the demo this has to be able to publish scales
// cubes.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

namespace engine::scene {

	// One thing to draw, flat and copyable.
	//
	// Flat because the day a world is a process this has to survive being
	// memcpy'd, so no field of it may be a pointer or own an allocation.
	//
	// Interpolation has already happened by the time one of these exists: this
	// is where the thing *is* for the frame being drawn, not where it was at a
	// tick boundary. A consumer that re-interpolated would be interpolating
	// twice.
	//
	// @since v0.4
	struct DrawInstance {
		// World-space placement for this frame.
		core::CFrame Frame;

		// How big it is, as a half-extent on each local axis — the same form
		// `Bounds` carries, so the producer copies rather than converts.
		core::Vector3 HalfExtent{0.5f, 0.5f, 0.5f};

		// Flat multiplier over the material.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};

		// Which mesh, by name. Invalid means the consumer's default.
		core::Name Mesh;

		// Which material, by name. Invalid means the consumer's default.
		core::Name Material;
	};
}
