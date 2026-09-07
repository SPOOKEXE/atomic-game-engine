#pragma once

#include <engine/core/types/Vector3.hpp>

#include <glm/glm.hpp>

#include <cmath>

namespace engine::render {
	// A pane is a slab, but its capture clips at one authored plane. Intersect
	// the viewing ray with that plane for both captured-image sampling and draw depth.
	inline glm::mat4 PortalImageSampling(
		const glm::mat4 &sampling,
		const core::Vector3 &eye,
		const core::Vector3 &centre,
		const core::Vector3 &normal
	) {
		const float distance = (eye - centre).Dot(normal);
		if (!std::isfinite(distance) || std::abs(distance) < 0.0001f) return sampling;
		const glm::vec4 origin{eye.X, eye.Y, eye.Z, 1};
		const glm::vec4 plane{normal.X, normal.Y, normal.Z, -normal.Dot(centre)};
		// Homogeneous ray-plane intersection preserves perspective interpolation
		// and the original positive clip-w on either side of the mouth.
		return sampling * (glm::mat4{1} - glm::outerProduct(origin, plane) / distance);
	}
}
