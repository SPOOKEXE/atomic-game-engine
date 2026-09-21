#pragma once

// Shared rigid-body inertia calculations.
//
// Contact impulses and persistent torques must use the same shape tensor. A
// second copy would make a body turn differently under a script load than it
// does under a contact.
//
// @tier L8 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>

#include <array>

namespace engine::physics {

	// The inverse of each principal moment for a collider of `mass` kilograms.
	core::Vector3 InverseInertiaOf(const scene::Collider &collider, float mass);

	// Applies a world-space torque to a diagonal inverse tensor whose principal
	// axes are expressed in world space.
	core::Vector3 AngularAcceleration(
		const std::array<core::Vector3, 3> &principalAxes,
		const core::Vector3 &inverseInertia,
		const core::Vector3 &torque
	);
}
