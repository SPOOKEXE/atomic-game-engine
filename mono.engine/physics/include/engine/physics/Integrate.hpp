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
	// | entities | `Each` | `IntegrateMotion` | |
	// |---|---|---|---|
	// | 1 000 | 3.7 us | 3.8 us | runs inline |
	// | 4 000 | 14.7 us | 14.8 us | runs inline |
	// | 8 000 | 29.3 us | 29.5 us | runs inline - the crossover |
	// | **12 000** | **44.0 us** | **23.9 us** | 1.8x faster |
	// | 20 000 | 73.0 us | 27.9 us | 2.6x faster |
	// | 100 000 | 365 us | 65.7 us | 5.6x faster |
	//
	// **512 was the number at `-O2` and it is 1024 at `-O3`, which is the whole
	// argument for re-taking a crossover rather than inheriting one.** The
	// serial column halved when the build changed level; the pool's handover -
	// 31 us, measured empty by `engine.parallel.bench.dispatch` - did not move,
	// so the row count that repays it doubled. At 512 the floor sat at 4096 rows
	// and a six-thousand-row world dispatched into a 1.3x loss. The wider range
	// is worth something of its own above the floor: every figure from 12 000 up
	// is 9 to 18 per cent better than the same run at 512, because a range costs
	// about 95 ns to hand out whatever is in it.
	//
	// `Jobs::MINIMUM_GRAINS` is 8, so a grain also sets the count below which
	// the whole span runs inline whatever the pool is doing: 1024 times 8 is
	// 8192 rows, which is where the crossover measured. The two numbers are the
	// same number on purpose, and the first three rows above are inline runs -
	// they are the serial figure, and the difference is the measurement.
	//
	// The default grain of 4096 would put that floor at 32768 rows and refuse
	// to dispatch anything below it. Measured at twenty thousand entities, that
	// is 73.5 us against this grain's 27.3 us, for the same body: the whole
	// difference is a dispatch that the default declined to make.
	inline constexpr size_t INTEGRATE_GRAIN = 1024;

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
	// The delta is `Store::Time().Delta`, the fixed tick, never a measured frame
	// time. A tick has to be a function of its state or a recording stops
	// replaying.
	//
	// @param store The world to advance.
	// @tick
	void IntegrateMotion(ecs::Store &store);
}
