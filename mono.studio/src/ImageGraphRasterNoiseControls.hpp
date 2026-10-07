#pragma once

#include "ImageGraphNoiseControls.hpp"

namespace studio::detail {
	inline bool IsImageGraphRasterNoiseSelector(std::string_view nodeType, std::string_view property) {
		return property == "output_type" && engine::imagegraph::IsNoiseImageGenerator(nodeType);
	}

	inline std::optional<bool> DrawImageGraphRasterNoiseChoice(
		std::string_view nodeType, std::string_view property, engine::imagegraph::EnumValue &value
	) {
		if (!IsImageGraphRasterNoiseSelector(nodeType, property)) return std::nullopt;
		return DrawImageGraphNoiseChoice("value.sample_noise", property, value);
	}
}
