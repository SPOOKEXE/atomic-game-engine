#include "WorldResource.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>

#include <algorithm>
#include <limits>

namespace engine::physics {

	double SanePhysicsRate(double stepsPerSecond) {
		// **A positive test rather than `<= 0`, so a NaN lands on zero**, and a
		// ceiling above it so an infinity lands on something an `int32_t` can
		// hold. Both arrive from a game file and from a snapshot, which
		// `docs/CODE_QUALITY.md` §7 calls hostile - and the failure they would
		// otherwise reach is undefined behaviour in `BeginPhysicsTick`'s cast
		// rather than a strange number of steps.
		//
		// One function, called by the setter and by the snapshot reader, so
		// the two cannot come to disagree about what a rate may be.
		return std::min(stepsPerSecond > 0.0 ? stepsPerSecond : 0.0, PhysicsClock::MAXIMUM_RATE);
	}

	void SetPhysicsTickRate(ecs::Store &store, double stepsPerSecond) {
		PhysicsClock *clock = PreparedClockMutable(store);
		if (clock == nullptr) {
			return;
		}

		// **The accumulator is kept.** A rate raised between two ticks should
		// make the next step shorter, not throw away the simulated time the
		// world has already been charged for - dropping it is a world that
		// silently skips forward whenever an author touches a slider.
		clock->Rate = SanePhysicsRate(stepsPerSecond);
	}

	void SetPhysicsPaused(ecs::Store &store, bool paused) {
		PhysicsClock *clock = PreparedClockMutable(store);
		if (clock == nullptr) return;
		clock->Paused = paused;
		clock->Owed = 0;
		clock->Stepping = false;
		clock->StepInTick = 0;
	}

	bool IsPhysicsPaused(const ecs::Store &store) {
		const PhysicsClock *clock = PreparedClock(store);
		return clock != nullptr && clock->Paused;
	}

	double PhysicsTickRate(const ecs::Store &store) {
		const PhysicsClock *clock = PreparedClock(store);
		return clock != nullptr ? clock->Rate : 0.0;
	}

	float PhysicsStepSeconds(const ecs::Store &store) {
		const PhysicsClock *clock = PreparedClock(store);
		if (clock == nullptr || !clock->Stepping) {
			// No pipeline around this call, so the world's tick is the step -
			// which is what every step meant before this clock existed and what
			// a suite calling one directly still means.
			return store.Time().Delta;
		}
		return clock->Delta;
	}

	const PhysicsClock *PhysicsClockOf(const ecs::Store &store) {
		return PreparedClock(store);
	}

	void BeginPhysicsTick(ecs::Store &store) {
		PhysicsClock *clock = PreparedClockMutable(store);
		if (clock == nullptr) {
			return;
		}

		const float tick = store.Time().Delta;

		clock->StepInTick = 0;
		clock->Stepping = false;
		if (clock->Paused) {
			clock->Owed = 0;
			return;
		}

		if (!(clock->Rate > 0.0)) {
			// Following the world, which is one step of exactly the tick. Kept
			// as its own branch rather than falling out of the arithmetic
			// below: `Accumulator / (1 / rate)` at the world's own rate is a
			// division and a multiplication whose remainder is not reliably
			// zero, and a world that said nothing about physics must not drop a
			// step every few minutes because of rounding.
			clock->Accumulator = 0.0;
			clock->Owed = 1;
			return;
		}

		clock->Accumulator += static_cast<double>(tick);

		const double interval = 1.0 / clock->Rate;
		const double owedSeconds = clock->Accumulator / interval;
		if (!(owedSeconds > 0.0)) {
			clock->Owed = 0;
			return;
		}
		if (!std::isfinite(owedSeconds) ||
			owedSeconds >= static_cast<double>(PhysicsClock::MAXIMUM_STEPS_PER_TICK + 1)) {
			const uint64_t owed = owedSeconds >= static_cast<double>(std::numeric_limits<uint64_t>::max())
									  ? std::numeric_limits<uint64_t>::max()
									  : static_cast<uint64_t>(owedSeconds);
			const uint64_t dropped =
				owed > PhysicsClock::MAXIMUM_STEPS_PER_TICK ? owed - PhysicsClock::MAXIMUM_STEPS_PER_TICK : 0;
			clock->DroppedSteps = std::numeric_limits<uint64_t>::max() - clock->DroppedSteps < dropped
									  ? std::numeric_limits<uint64_t>::max()
									  : clock->DroppedSteps + dropped;
			clock->Accumulator = 0.0;
			clock->Owed = PhysicsClock::MAXIMUM_STEPS_PER_TICK;
			return;
		}
		auto owed = static_cast<int32_t>(owedSeconds);
		clock->Accumulator -= owed * interval;

		clock->Owed = owed;
	}

	bool BeginPhysicsStep(ecs::Store &store) {
		PhysicsClock *clock = PreparedClockMutable(store);
		if (clock == nullptr) {
			return false;
		}
		if (clock->Paused) {
			clock->Owed = 0;
			clock->Stepping = false;
			return false;
		}

		if (clock->Owed <= 0) {
			clock->Stepping = false;
			return false;
		}

		clock->Owed--;
		clock->Delta = clock->Rate > 0.0 ? static_cast<float>(1.0 / clock->Rate) : store.Time().Delta;
		clock->Stepping = true;
		clock->StepInTick++;
		clock->Steps++;
		return true;
	}

	bool FirstPhysicsStepOfTick(const ecs::Store &store) {
		const PhysicsClock *clock = PreparedClock(store);
		return clock == nullptr || clock->StepInTick <= 1;
	}
}
