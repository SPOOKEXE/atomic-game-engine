#pragma once

#include <algorithm>
#include <cstddef>

namespace studio {
	// An empty result closes the popup and resets its selection.
	inline int ClampCompletionChoice(int choice, size_t count) {
		if (count == 0) return 0;
		return static_cast<int>(std::min(static_cast<size_t>(std::max(choice, 0)), count - 1));
	}
}
