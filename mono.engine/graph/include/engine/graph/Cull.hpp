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
	// on screen.
	//
	// **That sweep runs `rounds` times, and one was wrong from v0.15.** It used
	// to run exactly once, on the argument that a pane buried two bounces deep
	// could light up a frame later on the same one-frame budget the surface pass
	// ran on. `D00112` removed that budget: the surface pass now runs
	// `Renderer::SetSurfaceBounces` times and resolves a chain that deep *inside*
	// the frame. So the render went to `n` levels while this went to one, and the
	// levels past the first were culled rather than merely late.
	//
	// **The symptom was a camera angle, which is what made it hard to see.** A
	// mirror directly on screen reveals what it can see for free. Turn far enough
	// that it leaves the frustum and everything it was revealing drops a level —
	// so reflections that were fine at one angle vanish at another, with the
	// geometry itself untouched.
	//
	// A round takes a fresh snapshot rather than marking as it walks, so the
	// answer still cannot depend on the order surfaces arrive in — that property
	// is what the snapshot was always for, and it survives being iterated.
	//
	// **And how much of the screen each pane covers**, which is what decides how
	// many texels its image needs. A surface camera is fitted to its pane, so
	// the texture maps one-to-one onto the pane's screen footprint: a pane
	// filling half the screen wants half the screen's pixels, and giving it a
	// fixed thousand-and-twenty-four whatever it covers is what makes a portal
	// go blocky as you walk up to it. Coverage is reported as a fraction of the
	// viewport's larger axis, so a caller multiplies rather than reasons.
	//
	// **One for a pane the camera is inside or behind.** A box straddling the
	// camera's own plane has no bounded projection, and the useful answer there
	// is "as much as you have" rather than a number produced by dividing by a
	// negative `w`.
	//
	// @param instances The whole draw list, culled or not.
	// @param camera    The viewer's world-to-clip matrix. The frustum is taken
	//                  from it rather than passed beside it, so the two cannot
	//                  describe different cameras.
	// @param surfaces  The surface cameras this frame accepted.
	// @param visible   Indexed by slot, cleared then filled. Must be at least as
	//                  long as the highest index in `surfaces`, plus one.
	// @param coverage  Indexed by slot, cleared then filled with 0..1. Optional;
	//                  pass an empty span to skip the projection entirely.
	// @param rounds    How many levels of surface-seen-in-surface to follow.
	//                  **Pass the renderer's bounce count**: a level the pass
	//                  will draw and this did not mark is a level that is culled
	//                  rather than stale. Zero and one both mean "directly on
	//                  screen, plus one hop", which is the pre-v0.15 behaviour.
	//                  Stops early once a round grants nothing, so asking for
	//                  more than the scene has costs one extra sweep of a
	//                  sixteen-entry array.
	// @return How many slots came back visible.
	// @since v0.15
	size_t VisibleSurfaces(
		std::span<const scene::DrawInstance> instances,
		const glm::mat4 &camera,
		std::span<const SurfaceEye> surfaces,
		std::span<bool> visible,
		std::span<float> coverage = {},
		size_t rounds = 1
	);

	// Whether one rectangle is in front of one camera at all.
	//
	// **The recursive portal pass' cull, and it is per portal *per level*.** A
	// surface's visibility is decided once against the eye because a surface
	// camera is placed from the eye; a portal's sub-camera is derived from
	// whichever camera the recursion is currently at, so "can this pane be seen"
	// has to be asked again at every level — and answering it is what stops the
	// cost being `portals ^ depth` scene renders in a scene where most holes are
	// behind you. CodeParade's demo spends a GPU occlusion query per portal per
	// level on exactly this; this is the same question asked on the CPU.
	//
	// **A rectangle rather than a draw instance**, because at depth the pane is
	// not where the draw list says it is: the sub-camera lives in the warped
	// space, and what has to be tested there is the *mapped* rectangle. The
	// caller maps it and hands over four corners' worth of description.
	//
	// **A box around the rectangle, not the rectangle itself.** The box is at
	// least the quad, so the error is always towards drawing — which is the
	// direction culling has to err in, since a pane wrongly dropped is a hole
	// that goes black for a frame.
	//
	// @param camera The camera's world-to-clip matrix.
	// @param centre The rectangle's middle, in world space.
	// @param first  One half-axis, so `centre ± first ± second` is the corners.
	// @param second The other.
	// @return Whether any of it may be on screen.
	// @since v0.15
	bool VisiblePane(
		const glm::mat4 &camera,
		const core::Vector3 &centre,
		const core::Vector3 &first,
		const core::Vector3 &second
	);
}
