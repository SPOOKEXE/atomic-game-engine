#pragma once

#include <algorithm>
#include <cmath>

namespace engine::render {

	// Converts one authored display channel into the HDR working value that the
	// default ACES and gamma curve maps back to it.
	//
	// Fog is authored as a final display colour but blended into the HDR frame.
	// Applying the forward curve to the authored value again brightens it, so the
	// renderer performs the inverse once per view before the blend.
	inline float WorkingFromDisplay(float display) {
		const float mapped = std::pow(std::clamp(display, 0.0f, 1.0f), 2.2f);
		const float quadratic = mapped * 2.43f - 2.51f;
		const float linear = mapped * 0.59f - 0.03f;
		const float constant = mapped * 0.14f;
		const float discriminant = std::max(linear * linear - 4.0f * quadratic * constant, 0.0f);
		return std::max((-linear - std::sqrt(discriminant)) / (2.0f * quadratic), 0.0f);
	}
}
