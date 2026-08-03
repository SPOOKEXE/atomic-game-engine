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

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// The smallest run of rows worth handing to another worker.
	//
	// **Measured, in the `bench` preset**, by `benchmarks/Integrate.cpp`, which
	// is also where to re-take it. The body carries a whole `core::CFrame` — a
	// position add, a quaternion product and a normalise, about forty flops and
	// a reciprocal square root — so it is nowhere near the cheap body
	// `parallel::Jobs::DEFAULT_GRAIN` of 4096 was chosen for.
	//
	// | entities | `Each` | `IntegrateMotion` | |
	// |---|---|---|---|
	// | 1 000 | 3.7 us | 3.8 us | runs inline |
	// | 4 000 | 14.7 us | 14.8 us | runs inline |
	// | 8 000 | 29.3 us | 29.5 us | runs inline — the crossover |
	// | **12 000** | **44.0 us** | **23.9 us** | 1.8x faster |
	// | 20 000 | 73.0 us | 27.9 us | 2.6x faster |
	// | 100 000 | 365 us | 65.7 us | 5.6x faster |
	//
	// **512 was the number at `-O2` and it is 1024 at `-O3`, which is the whole
	// argument for re-taking a crossover rather than inheriting one.** The
	// serial column halved when the build changed level; the pool's handover —
	// 31 us, measured empty by `engine.parallel.bench.dispatch` — did not move,
	// so the row count that repays it doubled. At 512 the floor sat at 4096 rows
	// and a six-thousand-row world dispatched into a 1.3x loss. The wider range
	// is worth something of its own above the floor: every figure from 12 000 up
	// is 9 to 18 per cent better than the same run at 512, because a range costs
	// about 95 ns to hand out whatever is in it.
	//
	// `Jobs::MINIMUM_GRAINS` is 8, so a grain also sets the count below which
	// the whole span runs inline whatever the pool is doing: 1024 times 8 is
	// 8192 rows, which is where the crossover measured. The two numbers are the
	// same number on purpose, and the first three rows above are inline runs —
	// they are the serial figure, and the difference is the measurement.
	//
	// The default grain of 4096 would put that floor at 32768 rows and refuse
	// to dispatch anything below it. Measured at twenty thousand entities, that
	// is 73.5 us against this grain's 27.3 us, for the same body: the whole
	// difference is a dispatch that the default declined to make.
	inline constexpr size_t INTEGRATE_GRAIN = 1024;

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
