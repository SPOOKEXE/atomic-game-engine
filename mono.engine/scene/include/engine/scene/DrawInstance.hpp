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

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

		// How much of what is behind shows through, 0 to 1.
		//
		// **The field is cheap and the ordering is not.** A non-zero value puts
		// this instance in a second pass, sorted back-to-front per view — which
		// is the first thing the renderer does that depends on *which camera is
		// looking*, and the reason this arrived with the pass rather than ahead
		// of it. See `SortForDrawing`.
		float Transparency = 0.0f;
	};

	// Produces the order a draw list should be submitted in.
	//
	// **An order rather than a sort in place**, because the consumer holds a
	// `std::span<const DrawInstance>` — a view published by a world it does not
	// own, which may be another process's memory. Writing an index list also
	// costs four bytes an instance instead of moving eighty.
	//
	// **Why the renderer cannot just draw them in any order.** Opaque geometry
	// writes depth, so whatever is nearest wins whichever order it arrived in.
	// A blended fragment does not replace what is behind it — it mixes with
	// whatever is already in the target — so drawing a near pane before a far
	// one blends the far one *into* a pixel that should have hidden it. The
	// result is a window that looks right from one side of the room and wrong
	// from the other, which reads as a shader bug rather than an ordering one.
	//
	// **Back to front, by squared distance from the eye.** The square root is
	// not taken: it is monotonic, so it cannot change an ordering, and this runs
	// over every transparent instance every frame per view.
	//
	// **A stable sort**, so two panes at the same distance keep the order the
	// world produced them in. An unstable one would swap them from frame to
	// frame as the comparison fell either way, and a recording would stop
	// replaying — which is a determinism failure arriving through a renderer.
	//
	// Here rather than in `render` because it is arithmetic over a `shared`
	// type, and a headless host publishing a view has the same reason to order
	// it. `render` is where the *pipeline* lives; this is where the list does.
	//
	// @param instances The draw list.
	// @param eye       Where the view is, in world space.
	// @param order     Filled in with indices into `instances`. Cleared first.
	// @return How many indices at the front of `order` name opaque instances.
	size_t OrderForDrawing(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	);

	// Whether an instance needs the blended pass.
	//
	// **Not `> 0`, and the epsilon is the point.** A `Transparency` of a
	// millionth is visually opaque and costs a sort, a pipeline switch and the
	// loss of depth writes; a value that small is arithmetic noise from a tween
	// rather than an author's intent.
	//
	// @param instance The instance to classify.
	// @return `true` when it belongs in the transparent pass.
	bool IsTransparent(const DrawInstance &instance);
}
