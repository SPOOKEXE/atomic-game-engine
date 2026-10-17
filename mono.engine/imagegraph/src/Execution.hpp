#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	inline uint64_t WorkPerPixel(const Operation &operation) {
		if (std::holds_alternative<Blend>(operation)) return 2;
		if (const auto *resize = std::get_if<Resize>(&operation))
			return resize->Filter == Sampling::Bilinear ? 4 : 1;
		if (const auto *transform = std::get_if<Transform>(&operation))
			return transform->Filter == Sampling::Bilinear ? 4 : 1;
		return 1;
	}
	inline ImageExtent Extent(const Operation &operation, const ImageExtent &input) {
		return std::visit(
			[&](const auto &value) -> ImageExtent {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
							  std::is_same_v<T, Crop> || std::is_same_v<T, Transform>)
					return {value.Width, value.Height, size_t(value.Width) * value.Height * 4};
				else
					return input;
			},
			operation
		);
	}
}
