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
	// | 1 000 | 4.1 us | 3.8 us | runs inline |
	// | 4 000 | 16.0 us | 14.9 us | runs inline |
	// | **20 000** | **80.2 us** | **28.5 us** | 2.8x faster |
	// | 100 000 | 403 us | 64.6 us | 6.2x faster |
	//
	// **The old number does not carry forward.** `Store::EachParallel` records
	// a crossover near sixty to eighty thousand rows for a body of three float
	// multiply-adds. This body carries a `CFrame` and the crossover is at four
	// thousand — the row got expensive enough to repay the handover more than
	// an order of magnitude sooner. `parallel/AGENTS.md` says a default grain is
	// a guess about how expensive one row is, and this is the row that stopped
	// being cheap.
	//
	// `Jobs::MINIMUM_GRAINS` is 8, so a grain also sets the count below which
	// the whole span runs inline whatever the pool is doing: 512 times 8 is
	// 4096 rows, which is where the crossover measured. The two numbers are the
	// same number on purpose, and the first two rows above are inline runs —
	// they are the serial figure, and the difference is the measurement.
	//
	// The default grain of 4096 would put that floor at 32768 rows and refuse
	// to dispatch anything below it. Measured at twenty thousand entities, that
	// is 79.2 us against this grain's 30.6 us, for the same body: the whole
	// difference is a dispatch that the default declined to make.
	inline constexpr size_t INTEGRATE_GRAIN = 512;

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
