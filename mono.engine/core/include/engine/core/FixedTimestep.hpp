#pragma once

// A fixed simulation rate under a variable frame rate.
//
// Without this, simulation advances by however long the frame took — so the
// same game behaves differently at 30 fps and 300, a recorded run does not
// replay, and every frame-time measurement conflates two rates that have no
// reason to match.
//
// The frame feeds it elapsed time and gets back how many simulation ticks to
// run. Whatever is left over is `Alpha`, and rendering interpolates by it:
// drawing at tick positions while rendering faster than the tick rate judders,
// visibly.
//
//     const int ticks = Timestep.Advance(frameSeconds);
//     for (int tick = 0; tick < ticks; tick++) {
//         Simulate(Timestep.Delta());      // always the same delta
//     }
//     Render(Timestep.Alpha());            // 0..1 between the last two ticks
//
// @tier L0 · shared

#include <cstdint>

namespace engine::core {

	// Accumulates frame time into deterministic, fixed-duration simulation ticks.
	class FixedTimestep {
	  public:
		// A stall longer than this many ticks is given up on rather than
		// caught up. Eight at 60 Hz is about 130 ms.
		static constexpr int MAXIMUM_TICKS_PER_FRAME = 8;

		// Creates a timestep at `ticksPerSecond`, using 1 Hz when the rate is not positive.
		explicit FixedTimestep(double ticksPerSecond = 60.0);

		// Changes the rate immediately without discarding accumulated time.
		//
		// A rate that is zero or negative becomes 1 Hz. Rate(), Delta(), and Alpha()
		// use the new rate as soon as this returns.
		void SetRate(double ticksPerSecond);

		// The current number of simulation ticks per second.
		double Rate() const {
			return TicksPerSecond;
		}

		// Seconds per tick. This is the delta every simulation system sees, and it
		// stays constant until the next SetRate().
		float Delta() const {
			return static_cast<float>(1.0 / TicksPerSecond);
		}

		// How many ticks to run for this frame. Zero is normal and correct:
		// a 300 fps frame usually advances no tick at all.
		//
		// Positive frame time is accumulated. If more than
		// MAXIMUM_TICKS_PER_FRAME are owed, the excess is dropped and no remainder
		// is carried into the next frame.
		int Advance(float frameSeconds);

		// Where the render sits between the last tick and the next, 0..1.
		float Alpha() const;

		// The number of ticks returned by Advance() since construction or Reset().
		uint64_t TotalTicks() const {
			return Ticks;
		}

		// Ticks abandoned to the clamp above.
		//
		// Worth surfacing rather than swallowing: it is the difference between
		// "the machine is slow" and "simulation is silently running slower than
		// its stated rate", and those need different responses.
		uint64_t Dropped() const {
			return DroppedTicks;
		}

		// Clears accumulated time and tick counts without changing the rate.
		void Reset();

	  private:
		double TicksPerSecond = 60.0;
		double Accumulator = 0.0;
		uint64_t Ticks = 0;
		uint64_t DroppedTicks = 0;
	};
}
