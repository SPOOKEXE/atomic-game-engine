#pragma once

// Which of a world's draw instances a view can actually see.
//
// **The cheapest frame is the one that draws less.** A world of ten thousand
// parts seen through a seventy-degree field of view usually has a few hundred on
// screen, and every one of the rest costs a matrix, eighty bytes of transfer and
// a vertex-shader invocation per vertex before the rasteriser throws it away.
//
// The result is an **index list**, for the reason `scene::OrderForDrawing`
// produces one: the draw list is a `span` of something the consumer does not
// own, four bytes an instance beats moving eighty, and the two lists compose —
// cull first, order second, over the survivors.
//
// @tier L9 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::graph {

	// The world-space box a draw instance occupies.
	//
	// **`AABB::FromOrientedBox`, not a centre and the half-extent.** A rotated
	// cube reaches further on every axis than its own half-extent, and a bound
	// smaller than the shape is a part that vanishes as it turns near the screen
	// edge — the exact bug the box test is written to be conservative against,
	// arriving through the bound instead of the test.
	//
	// @param instance The instance to bound.
	// @return Its world-space box.
	core::AABB BoundsOf(const scene::DrawInstance &instance);

	// Fills `visible` with the indices of the instances a frustum may see.
	//
	// **The ascending order of `visible` is part of the contract.** It is the
	// world's own order, and `scene::OrderForDrawing` leaves the opaque head in
	// it so that a recorded frame replays as itself. A pass that filled this
	// list in any other order would be correct on screen and wrong on replay.
	//
	// A caller that also needs the whole list's bound — the shadow fit does —
	// should call `Shadow.hpp`'s `CullAndBound` rather than this and
	// `BoundsOfAll`. Both walks derive the same `BoundsOf` per instance, and
	// that bound is the expensive half of either.
	//
	// @param instances The draw list.
	// @param frustum   The view.
	// @param visible   Filled in with indices into `instances`. Cleared first.
	// @return How many are visible.
	size_t Cull(
		std::span<const scene::DrawInstance> instances, const Frustum &frustum, std::vector<uint32_t> &visible
	);

	// One surface camera, as much of it as deciding visibility needs.
	//
	// @since v0.15
	struct SurfaceEye {
		// World to that camera's clip space, which is what a frustum comes out
		// of.
		glm::mat4 ViewProjection{1.0f};

		// The slot it renders into, matching `scene::DrawInstance::Surface` on
		// whatever samples it.
		int8_t Index = 0;
	};

	// Which surface slots have a pane something can currently see.
	//
	// **The other half of "should this surface redraw", and the half the render
	// pass did not have.** A signature answers whether the image *changed*;
	// nothing answered whether it is *looked at*, so a room of mirrors redrew
	// every one of them on every frame anything moved — including the ones
	// behind the viewer and the ones a wall stands in front of. CodeParade's
	// non-Euclidean demo spends a GPU occlusion query per portal per recursion
	// level on exactly this question, which is how it affords four levels of
	// them; this is the same question asked on the CPU against boxes the culler
	// has already been deriving.
	//
	// **A pane and not a camera.** A `SurfaceEye` says where the camera is, and
	// for a portal that is the far room — what has to be on screen is the pane,
	// and the only description of a pane here is the instance that samples the
	// slot. A slot with no instance naming it is a surface nothing samples, and
	// comes back invisible.
	//
	// **Two sweeps, and deliberately not a fixed point.** A pane visible only
	// *inside another mirror* is a real case — two facing panes, a portal seen
	// through a portal — so the camera frustum alone would freeze it. The second
	// sweep unions in the panes that are visible from a surface which is itself
	// on screen. Iterating that to closure would spend the saving this exists
	// for; one pass means a pane buried two bounces deep lights up a frame
	// later, which is the same one-frame budget the surface pass already runs
	// on.
	//
	// @param instances The whole draw list, culled or not.
	// @param camera    The viewer's frustum.
	// @param surfaces  The surface cameras this frame accepted.
	// @param visible   Indexed by slot, cleared then filled. Must be at least as
	//                  long as the highest index in `surfaces`, plus one.
	// @return How many slots came back visible.
	// @since v0.15
	size_t VisibleSurfaces(
		std::span<const scene::DrawInstance> instances,
		const Frustum &camera,
		std::span<const SurfaceEye> surfaces,
		std::span<bool> visible
	);
}
