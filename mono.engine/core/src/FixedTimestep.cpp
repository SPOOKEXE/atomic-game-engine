#include <engine/core/FixedTimestep.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::core {

	namespace {
		bool ValidRate(double rate) {
			if (!std::isfinite(rate) || rate <= 0.0) return false;
			const double seconds = 1.0 / rate;
			if (!std::isfinite(seconds) || seconds > std::numeric_limits<float>::max()) return false;
			const float delta = static_cast<float>(seconds);
			return std::isfinite(delta) && delta > 0.0f;
		}
	}

	FixedTimestep::FixedTimestep(double ticksPerSecond) {
		SetRate(ticksPerSecond);
	}

	void FixedTimestep::SetRate(double ticksPerSecond) {
		// A zero or negative rate would divide to infinity and then run
		// MAXIMUM_TICKS_PER_FRAME forever. Refusing it here beats debugging
		// that.
		TicksPerSecond = ValidRate(ticksPerSecond) ? ticksPerSecond : 1.0;
	}

	int FixedTimestep::Advance(float frameSeconds, int maximumTicks) {
		if (frameSeconds > 0.0f) {
			Accumulator += static_cast<double>(frameSeconds);
		}

		const double delta = 1.0 / TicksPerSecond;
		const double wholeTicks = std::floor(Accumulator / delta);
		if (!(wholeTicks >= 1.0)) {
			return 0;
		}

		const int ceiling = std::clamp(maximumTicks, 1, MAXIMUM_TICKS_PER_FRAME);
		int ticks;
		if (wholeTicks > ceiling) {
			// Drop the excess rather than carrying it. Carrying it is the
			// death spiral: the frame after a stall runs the maximum, takes
			// longer than a frame to do it, and arrives at the next one with
			// even more owed. Simulation time then falls permanently behind
			// wall time and never recovers.
			//
			// Giving up means the world skipped forward. That is visible and
			// recoverable; the spiral is neither.
			const double dropped = wholeTicks - static_cast<double>(ceiling);
			const uint64_t remaining = std::numeric_limits<uint64_t>::max() - DroppedTicks;
			if (!std::isfinite(dropped) || dropped >= static_cast<double>(remaining)) {
				DroppedTicks = std::numeric_limits<uint64_t>::max();
			} else {
				DroppedTicks += static_cast<uint64_t>(dropped);
			}
			ticks = ceiling;
			Accumulator = 0.0;
		} else {
			ticks = static_cast<int>(wholeTicks);
			Accumulator -= ticks * delta;
		}

		const uint64_t advanced = static_cast<uint64_t>(ticks);
		Ticks = advanced > std::numeric_limits<uint64_t>::max() - Ticks ? std::numeric_limits<uint64_t>::max()
																		: Ticks + advanced;
		return ticks;
	}

	float FixedTimestep::Alpha() const {
		const double delta = 1.0 / TicksPerSecond;
		// Clamped because a rate change can leave the accumulator holding more
		// than one tick of the new, shorter delta for a single frame.
		return static_cast<float>(std::clamp(Accumulator / delta, 0.0, 1.0));
	}

	FixedTimestep::State FixedTimestep::Capture() const {
		return State{TicksPerSecond, Accumulator, Ticks, DroppedTicks};
	}

	bool FixedTimestep::Restore(const State &state) {
		if (!ValidRate(state.Rate) || !std::isfinite(state.Accumulator) || state.Accumulator < 0.0) {
			return false;
		}
		TicksPerSecond = state.Rate;
		Accumulator = state.Accumulator;
		Ticks = state.Ticks;
		DroppedTicks = state.DroppedTicks;
		return true;
	}

	void FixedTimestep::Reset() {
		Accumulator = 0.0;
		Ticks = 0;
		DroppedTicks = 0;
	}
}
