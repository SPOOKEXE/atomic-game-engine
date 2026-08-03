#pragma once

// One row of `IntegrateMotion`, in a header so the benchmark measures the same
// arithmetic the system runs rather than a copy of it that drifted.
//
// Private: nothing outside this module integrates one row at a time, and
// publishing it would invite a caller to step a single entity outside the tick,
// which is how a world stops being a function of its state.

#include <engine/core/types/CFrame.hpp>
#include <engine/scene/Components.hpp>

#include <glm/gtc/quaternion.hpp>

namespace engine::physics {

	// Advances one transform by one fixed tick of its velocity.
	//
	// The quaternion step is `q + 0.5 * w * q * dt`, which is the first-order
	// integral of a rotation whose world-space angular velocity is `w`. Written
	// as `(1 + 0.5 * dt * w) * q`, its magnitude is `sqrt(1 + (0.5 * dt *
	// |w|)^2)` times the magnitude of `q` — never zero, however fast the spin,
	// so the normalise below needs no guard against a zero quaternion.
	//
	// **Normalised every tick and not occasionally.** A first-order step leaves
	// the quaternion slightly off the unit sphere and the error compounds; a
	// `CFrame` whose rotation is not unit length scales what it transforms, so
	// the symptom is parts that slowly grow rather than anything that reads as
	// a rotation bug. The alternative — renormalising when the drift passes a
	// threshold — is a data-dependent branch in the hottest loop in the tick to
	// save one reciprocal square root.
	inline void IntegrateOne(scene::Transform &transform, const scene::Motion &motion, float delta) {
		core::CFrame &frame = transform.Frame;
		frame.Position = frame.Position + motion.Linear * delta;

		// glm::quat takes w first. `spin` is the pure quaternion of the
		// world-space angular velocity, so it multiplies on the left.
		const glm::quat rotation = frame.Rotation();
		const glm::quat spin{0.0f, motion.Angular.X, motion.Angular.Y, motion.Angular.Z};
		const glm::quat stepped = glm::normalize(rotation + (spin * rotation) * (0.5f * delta));

		frame.QuaternionX = stepped.x;
		frame.QuaternionY = stepped.y;
		frame.QuaternionZ = stepped.z;
		frame.QuaternionW = stepped.w;
	}
}
