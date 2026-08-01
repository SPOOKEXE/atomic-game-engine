#include <engine/core/FixedTimestep.hpp>

#include <algorithm>

namespace engine::core {

	FixedTimestep::FixedTimestep(double ticksPerSecond) {
		SetRate(ticksPerSecond);
	}

	void FixedTimestep::SetRate(double ticksPerSecond) {
		// A zero or negative rate would divide to infinity and then run
		// MAXIMUM_TICKS_PER_FRAME forever. Refusing it here beats debugging
		// that.
		TicksPerSecond = ticksPerSecond > 0.0 ? ticksPerSecond : 1.0;
	}

	int FixedTimestep::Advance(float frameSeconds) {
		if (frameSeconds > 0.0f) {
			Accumulator += static_cast<double>(frameSeconds);
		}

		const double delta = 1.0 / TicksPerSecond;
		auto ticks = static_cast<int>(Accumulator / delta);
		if (ticks <= 0) {
			return 0;
		}

		if (ticks > MAXIMUM_TICKS_PER_FRAME) {
			// Drop the excess rather than carrying it. Carrying it is the
			// death spiral: the frame after a stall runs the maximum, takes
			// longer than a frame to do it, and arrives at the next one with
			// even more owed. Simulation time then falls permanently behind
			// wall time and never recovers.
			//
			// Giving up means the world skipped forward. That is visible and
			// recoverable; the spiral is neither.
			DroppedTicks += static_cast<uint64_t>(ticks - MAXIMUM_TICKS_PER_FRAME);
			ticks = MAXIMUM_TICKS_PER_FRAME;
			Accumulator = 0.0;
		} else {
			Accumulator -= ticks * delta;
		}

		Ticks += static_cast<uint64_t>(ticks);
		return ticks;
	}

	float FixedTimestep::Alpha() const {
		const double delta = 1.0 / TicksPerSecond;
		// Clamped because a rate change can leave the accumulator holding more
		// than one tick of the new, shorter delta for a single frame.
		return static_cast<float>(std::clamp(Accumulator / delta, 0.0, 1.0));
	}

	void FixedTimestep::Reset() {
		Accumulator = 0.0;
		Ticks = 0;
		DroppedTicks = 0;
	}
}
