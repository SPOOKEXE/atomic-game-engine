#pragma once

#include <cstddef>

namespace studio::detail {
	// Each peer gets its own retry deadline so one blocked link cannot stall another.
	template <typename Queue, typename Send>
	void RetryV5Welcomes(Queue &pending, double nowSeconds, double retrySeconds, Send send) {
		for (size_t index = 0; index < pending.size();) {
			if (nowSeconds < pending[index].RetryAtSeconds) {
				++index;
				continue;
			}
			if (!send(pending[index])) {
				pending[index].RetryAtSeconds = nowSeconds + retrySeconds;
				++index;
				continue;
			}
			pending.erase(pending.begin() + static_cast<ptrdiff_t>(index));
		}
	}
}
