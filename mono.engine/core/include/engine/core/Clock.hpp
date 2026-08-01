#pragma once

// One clock, many tick rates.
//
// Worlds tick at different rates (L4) but they all read time from here, so that
// a recorded run replays identically: a tick is a function of its state and its
// inbox, never of how long the frame happened to take.
//
// @tier L0 · shared

#include <chrono>
#include <cstdint>

namespace engine::core {

	// Reads the process-wide monotonic clock without exposing its platform source.
	class Clock {
	  public:
		// Nanoseconds since an unspecified epoch, monotonic. Never jumps
		// backwards, and unaffected by the wall clock being corrected.
		static uint64_t Nanoseconds();

		// Seconds from the same unspecified monotonic epoch as Nanoseconds().
		static double Seconds() {
			return static_cast<double>(Nanoseconds()) / 1'000'000'000.0;
		}
	};

	// A frame timer. Owns the "what is the delta" question so that no subsystem
	// has to keep its own last-frame timestamp — two of those drift apart the
	// first time one of them is updated in a branch.
	class FrameClock {
	  public:
		// Advances to now and returns the elapsed seconds. Clamped, because a
		// breakpoint in a debugger is not a two-minute frame and simulation code
		// should not have to defend against one.
		float Tick();

		// Unclamped seconds elapsed since the first Tick(), which is time zero.
		double Now() const {
			return CurrentSeconds;
		}

		// The number of elapsed frames after the first Tick() established time zero.
		uint64_t Frame() const {
			return FrameIndex;
		}

		// The most recent clamped frame duration in seconds.
		float Delta() const {
			return DeltaSeconds;
		}

		// The largest frame duration simulation code receives, in seconds.
		static constexpr float MAXIMUM_DELTA = 0.25f;

	  private:
		uint64_t StartNanoseconds = 0;
		uint64_t PreviousNanoseconds = 0;
		uint64_t FrameIndex = 0;
		double CurrentSeconds = 0.0;
		float DeltaSeconds = 0.0f;
	};
}
