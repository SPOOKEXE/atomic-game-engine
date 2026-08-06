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
}
