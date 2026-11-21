#pragma once

// Moving what has a velocity.
//
// One system over `<Transform, const Motion>` and nothing else. **It must not
// load a mass**, and that is the whole reason `scene::Motion` and
// `scene::RigidBody` are separate components: a platform, a projectile and a
// demo cube all move and none of them needs a kilogram. Adding `RigidBody` to
// this query narrows it to the bodies that have one *and* loads three floats per
// row that the arithmetic never reads.
//
// @tier L8 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// The smallest run of rows worth handing to another worker.
	//
	// **Measured, in the `bench` preset**, by `benchmarks/Integrate.cpp`, which
	// is also where to re-take it. The body carries a whole `core::CFrame` - a
	// position add, a quaternion product and a normalise, about forty flops and
	// a reciprocal square root - so it is nowhere near the cheap body
	// `parallel::Jobs::DEFAULT_GRAIN` of 4096 was chosen for.
	//
	// | entities | `Each` | grain 512 | grain 384 | grain 256 |
	// |---|---|---|---|---|
	// | 4 000 | 19.6 us | 22.4 us inline | **6.9 us** | 7.5 us |
	// | 6 000 | 30.7 us | 9.8 us | 9.8 us | 9.5 us to 56 us |
	// | 8 000 | 35.7 us | 11.3 us | 10.8 us | 10.2 us to 25 us |
	// | 12 000 | 62.3 us | 13.9 us to 105 us | 14.6 us | 13.8 us to 74 us |
	// | 20 000 | 97.2 us | 19.8 us | 20.1 us | 19.9 us |
	// | 100 000 | 578 us | 82 us to 452 us | 81.8 us | 75 us to 370 us |
	//
	// Medians of three runs each against the lock-free `Jobs` handover, on a
	// machine other builds were loading; a range is the spread across runs.
	//
	// `Jobs::MINIMUM_GRAINS` is 8, so a grain also sets the count below which
	// the whole span runs inline whatever the pool is doing: 384 times 8 is
	// 3072 rows. At 512 that floor was 4096, and the 4000 row ran inline at
	// three times the dispatched cost. 256 measured no better above that and
	// far noisier, so the grain sits at the smallest one that held steady.
	//
	// The default grain of 4096 would put that floor at 32768 rows and refuse
	// to dispatch anything below it: 97 us against this grain's 20 us at
	// twenty thousand entities, for the same body.
	inline constexpr size_t INTEGRATE_GRAIN = 384;

	// Where a frame is `seconds` later, at a constant velocity.
	//
	// **The one implementation of what a velocity does to a `core::CFrame`.**
	// `IntegrateMotion` below is this function over a whole world at the fixed
	// tick delta; a replica placing a body the authority has stopped describing
	// is it over one row at whatever it has been left to guess with. Two
	// implementations would let a client integrate arithmetic the server does
	// not, which is a disagreement that only ever shows up as drift.
	//
	// **It returns a value and touches no store, which is what keeps the second
	// caller honest.** Stepping a single row *inside* a world is how a tick
	// stops being a function of its state, so this deliberately offers no way to
	// do it: a caller gets a pose and decides for itself whether that pose is
	// something to draw or something to store. `replication/AGENTS.md` says
	// which of those a replica is allowed.
	//
	// The quaternion step is the first-order integral of a rotation whose
	// world-space angular velocity is `w`:
	//
	// @code
	// q + 0.5 * w * q * dt   ==   (1 + 0.5 * dt * w) * q
	// @endcode
	//
	// Its magnitude is that of `q` scaled by the square root of one plus half
	// the tick times the spin rate, squared - never zero however fast the spin,
	// so the normalise below needs no guard against a zero quaternion.
	//
	// **Normalised every time and not occasionally.** A first-order step leaves
	// the quaternion slightly off the unit sphere and the error compounds; a
	// `CFrame` whose rotation is not unit length scales what it transforms, so
	// the symptom is parts that slowly grow rather than anything that reads as a
	// rotation bug. The alternative - renormalising when the drift passes a
	// threshold - is a data-dependent branch in the hottest loop in the tick, to
	// save one reciprocal square root.
	//
	// @param frame   Where the body was.
	// @param linear  Metres per second, in world space.
	// @param angular Radians per second, about each world axis.
	// @param seconds How long to advance for.
	// @return The advanced frame, its rotation renormalised.
	inline core::CFrame Advanced(
		const core::CFrame &frame, const core::Vector3 &linear, const core::Vector3 &angular, float seconds
	) {
		// glm::quat takes w first. `spin` is the pure quaternion of the
		// world-space angular velocity, so it multiplies on the left.
		const glm::quat rotation = frame.Rotation();
		const glm::quat spin{0.0f, angular.X, angular.Y, angular.Z};
		return core::CFrame{
			frame.Position + linear * seconds, glm::normalize(rotation + (spin * rotation) * (0.5f * seconds))
		};
	}

	// Advances every transform by its velocity, over one fixed tick.
	//
	// `Phase::Simulation`, parallel, over `<Transform, const Motion>`.
	//
	// **Angular velocity is integrated, not ignored.** `scene::Motion::Angular`
	// existed with nothing reading it until this system; the quaternion
	// derivative is `0.5 * w * q` with `w` the world-space angular velocity as a
	// pure quaternion, and the result is renormalised because integrating a
	// rotation with a first-order step does not preserve unit length and the
	// error compounds. A `CFrame` whose quaternion has drifted off the unit
	// sphere scales everything it transforms, which reads as parts slowly
	// growing rather than as a rotation bug.
	//
	// The delta is `PhysicsStepSeconds(store)`, never a measured frame time. A
	// tick has to be a function of its state or a recording stops replaying.
	// That is `Store::Time().Delta` for a world that steps physics at its own
	// tick rate, and the shorter or longer step of `Clock.hpp` for a world that
	// does not.
	//
	// @param store The world to advance.
	// @tick
	void IntegrateMotion(ecs::Store &store);
}
