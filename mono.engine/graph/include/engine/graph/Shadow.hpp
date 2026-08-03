#pragma once

// Where a directional light has to look from, and how wide.
//
// **A directional light has no position, and that is the whole problem.** The
// sun is a direction; a shadow map is a rendered image, and an image needs a
// camera. So one has to be invented — placed far enough back to see everything
// that casts, and framed tightly enough that the texels are not wasted on empty
// space.
//
// The trade is the only tuning decision here: a map fitted loosely wastes
// resolution and gives soft, blocky shadows; one fitted tightly clips casters
// that are outside the view but still shadow into it. This fits to the **whole
// scene** rather than to the camera's frustum, which is the conservative half
// of that trade — nothing is ever clipped, and the cost is resolution on a
// large world. Fitting to the frustum is the next thing to do here and it needs
// a stability pass with it, because a frame-to-frame refit makes shadow edges
// crawl.
//
// Arithmetic, so it lives in `graph` and is tested rather than looked at.
//
// @tier L9 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <glm/mat4x4.hpp>

#include <span>

namespace engine::graph {

	// The view-projection a shadow map is rendered with.
	//
	// **Orthographic, because the light's rays are parallel.** A perspective
	// projection here would make shadows converge toward a point the sun does
	// not have, and the error grows with distance from the middle of the map —
	// which reads as shadows that are right in the centre of a scene and wrong
	// at its edges.
	//
	// The box is fitted to `bounds` in the light's own space, so a scene that is
	// long in one direction gets a map that is long in that direction rather
	// than a square one sized to its diagonal.
	//
	// @param bounds    What the light must be able to see, in world space.
	// @param direction Which way the light travels. Need not be normalised;
	//                  a zero direction yields the identity, which shadows
	//                  nothing rather than shadowing everything.
	// @return `Projection * View` for the light.
	glm::mat4 FitDirectionalLight(const core::AABB &bounds, const core::Vector3 &direction);

	// The box every instance in a draw list occupies.
	//
	// What `FitDirectionalLight` is fitted to. Separate because a caller may
	// have a better answer — a world's authored `WorldBounds`, say — and
	// deriving one per frame from the draw list is the fallback rather than the
	// intent.
	//
	// @param instances The draw list.
	// @return The union of every instance's world box, or a unit box at the
	//         origin for an empty list.
	core::AABB BoundsOfAll(std::span<const scene::DrawInstance> instances);
}
