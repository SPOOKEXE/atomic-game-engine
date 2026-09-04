#pragma once

// Script-facing body motion operations.
//
// `scene::Motion` remains the compact storage component. These operations live
// in physics because writing a sleeping body must wake the physics world before
// returning the component to its moving archetype.
//
// @tier L8 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// The current world-space linear velocity, or zero for a static or sleeping
	// body.
	core::Vector3 LinearVelocity(const ecs::Store &store, ecs::Entity body);

	// The current world-space angular velocity, in radians per second, or zero
	// for a static or sleeping body.
	core::Vector3 AngularVelocity(const ecs::Store &store, ecs::Entity body);

	// Sets a simulated body's linear velocity and wakes it. Static bodies and
	// non-finite values are refused.
	bool SetLinearVelocity(ecs::Store &store, ecs::Entity body, const core::Vector3 &velocity);

	// Sets a simulated body's angular velocity and wakes it. Static bodies and
	// non-finite values are refused.
	bool SetAngularVelocity(ecs::Store &store, ecs::Entity body, const core::Vector3 &velocity);

	// Changes a dynamic body's linear velocity by impulse divided by its physical
	// mass, and wakes it. Kinematic and static bodies are refused.
	bool ApplyImpulse(ecs::Store &store, ecs::Entity body, const core::Vector3 &impulse);
}
