#include <engine/core/Clock.hpp>

#include <algorithm>

namespace engine::core {

	uint64_t Clock::Nanoseconds() {
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
	}

	float FrameClock::Tick() {
		const uint64_t now = Clock::Nanoseconds();

		if (StartNanoseconds == 0) {
			StartNanoseconds = now;
			PreviousNanoseconds = now;
			CurrentSeconds = 0.0;
			DeltaSeconds = 0.0f;
			FrameIndex = 0;
			return 0.0f;
		}

		const auto elapsed = static_cast<double>(now - PreviousNanoseconds) / 1'000'000'000.0;
		PreviousNanoseconds = now;

		DeltaSeconds = std::min(static_cast<float>(elapsed), MAXIMUM_DELTA);
		CurrentSeconds = static_cast<double>(now - StartNanoseconds) / 1'000'000'000.0;
		FrameIndex++;

		return DeltaSeconds;
	}
}
