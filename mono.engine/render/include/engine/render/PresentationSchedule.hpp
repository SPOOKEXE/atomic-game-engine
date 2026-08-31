#pragma once

// A monotonic deadline for presentation, independent of the update loop.
//
// @tier L12 · client

#include <chrono>
#include <cstdint>

namespace engine::render {

	// Limits presentation without limiting simulation.
	//
	// `Due` only observes the deadline. `Consume` advances it after a frame was
	// presented or deliberately skipped as unchanged, so a busy swapchain can be
	// retried without losing the frame that is owed.
	//
	// @since v0.19
	class PresentationSchedule {
	  public:
		// Monotonic clock and deadline types used by the schedule.
		//@{
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;
		//@}

		// Sets the maximum presentation rate. Zero is unlimited.
		void SetRate(uint32_t framesPerSecond);

		// Whether a presentation opportunity has arrived.
		//
		// The first call after construction or `SetRate` is due immediately.
		bool Due(TimePoint now);

		// Consumes the current opportunity and advances to the first future one.
		//
		// Missed intervals are dropped rather than replayed in a burst.
		void Consume(TimePoint now);

		// Time remaining before the next presentation opportunity.
		//
		// Zero means unlimited, not started, or already due.
		Clock::duration Remaining(TimePoint now) const;

		// The configured rate, or zero when every update is due.
		uint32_t Rate() const {
			return FramesPerSecond;
		}

	  private:
		uint32_t FramesPerSecond = 0;
		Clock::duration Period{};
		TimePoint Next{};
		bool Started = false;
	};
}
