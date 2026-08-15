#pragma once

// One row of `IntegrateMotion`, in a header so the benchmark measures the same
// arithmetic the system runs rather than a copy of it that drifted.
//
// Private, and it is the *store write* that is private rather than the
// arithmetic: `physics::Advanced` is public and returns a pose, because a caller
// outside a tick has to be able to place a body without being handed a way to
// step one. What stays in here is the half that writes a component, which is the
// half that would stop a world being a function of its state.

#include <engine/physics/Integrate.hpp>
#include <engine/scene/Components.hpp>

namespace engine::physics {

	// Advances one transform by one fixed tick of its velocity.
	inline void IntegrateOne(scene::Transform &transform, const scene::Motion &motion, float delta) {
		transform.Frame = Advanced(transform.Frame, motion.Linear, motion.Angular, delta);
	}
}
