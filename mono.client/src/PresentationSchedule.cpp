#include <client/PresentationSchedule.hpp>

namespace client {

	void PresentationSchedule::SetRate(uint32_t framesPerSecond) {
		FramesPerSecond = framesPerSecond;
		Period = framesPerSecond == 0
					 ? Clock::duration{}
					 : std::chrono::duration_cast<Clock::duration>(
						   std::chrono::duration<double>(1.0 / static_cast<double>(framesPerSecond))
					   );
		if (framesPerSecond > 0 && Period <= Clock::duration{}) {
			Period = Clock::duration{1};
		}
		Next = {};
		Started = false;
	}

	bool PresentationSchedule::Due(TimePoint now) {
		if (FramesPerSecond == 0) {
			return true;
		}
		if (!Started) {
			Next = now;
			Started = true;
		}
		return now >= Next;
	}

	void PresentationSchedule::Consume(TimePoint now) {
		if (FramesPerSecond == 0) {
			return;
		}
		if (!Started) {
			Next = now;
			Started = true;
		}

		// Advance from the old deadline, preserving phase, but go straight to the
		// first future slot. A slow render therefore drops obsolete opportunities
		// instead of making the update loop render a catch-up burst.
		const auto elapsed = now >= Next ? now - Next : Clock::duration{};
		const auto intervals = elapsed / Period + 1;
		Next += Period * intervals;
	}
}
