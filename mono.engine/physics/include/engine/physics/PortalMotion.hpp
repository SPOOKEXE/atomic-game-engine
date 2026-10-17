#pragma once

#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/SurfaceCameras.hpp>

namespace engine::physics {
	// Rigid motion of one portal mouth at a fixed physics sample.
	struct PortalMouthMotion {
		// Portal-mouth centre at the physics sample.
		core::Vector3 Centre;
		// Portal-mouth linear velocity.
		core::Vector3 Linear;
		// Portal-mouth angular velocity.
		core::Vector3 Angular;
	};

	// Maps body velocity relative to moving mouths at the same fixed sample.
	// Scale changes length and speed but leaves mass unchanged; scaled collider
	// dimensions make the solver's derived inertia scale by the square of scale.
	scene::Motion MapPortalMotion(
		const scene::SeamTransform &through,
		const core::Vector3 &sourcePosition,
		const scene::Motion &body,
		const PortalMouthMotion &source,
		const PortalMouthMotion &destination
	);
}
