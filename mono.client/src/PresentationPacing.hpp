#pragma once

#include <client/Options.hpp>

namespace client {

	// Hosts without display pacing retain a bounded service cadence. Ordinary
	// clients use the requested cap only when presentation is independent of VSync.
	[[nodiscard]] inline uint32_t PresentationRateOf(const Options &settings) {
		if (settings.PresentationSession != 0 || (settings.DataFactory && settings.Headless)) {
			return settings.MaximumFrameRate != 0 ? settings.MaximumFrameRate : 60;
		}
		return settings.Uncapped ? settings.MaximumFrameRate : 0;
	}
}
