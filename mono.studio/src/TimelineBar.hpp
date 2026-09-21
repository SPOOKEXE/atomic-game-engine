#pragma once

#include <algorithm>

namespace studio {
	struct TimelineBar {
		float Left = 0;
		float Right = 0;
	};

	// Keep tiny spans visible without extending beyond the timeline.
	inline TimelineBar FitTimelineBar(float startPixels, float durationPixels, float graphWidth) {
		const float width = std::max(graphWidth, 0.0f);
		const float minimum = std::min(width, 1.0f);
		const float left = std::clamp(startPixels, 0.0f, width - minimum);
		return {left, left + std::clamp(durationPixels, minimum, width - left)};
	}
}
