#pragma once

#include <optional>

namespace engine::render {
	// A body packet and its capture share the source clock. Captures without a
	// packet remain on the destination world's published presentation clock.
	inline double PortalPresentationSeconds(double destination, std::optional<double> source) {
		return source.value_or(destination);
	}
}
