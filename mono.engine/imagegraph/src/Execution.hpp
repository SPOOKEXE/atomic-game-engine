#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cmath>

namespace engine::imagegraph::detail {
	// Intermediate bytes per pixel for a document's precision.
	inline size_t StorageBytes(Precision precision) {
		return precision == Precision::Wide ? 8 : 4;
	}
	// Declared work per output pixel, checked against Limits::MaximumPixelWork.
	inline uint64_t WorkPerPixel(const Operation &operation) {
		return std::visit(
			[](const auto &value) -> uint64_t {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Blend>) return 2;
				if constexpr (std::is_same_v<T, Resize> || std::is_same_v<T, Transform> ||
							  std::is_same_v<T, Warp>)
					return value.Filter == Sampling::Bilinear ? 4 : 1;
				if constexpr (std::is_same_v<T, Noise>) return uint64_t(value.Octaves) * 4;
				if constexpr (std::is_same_v<T, Voronoi>)
					return value.Feature == VoronoiFeature::Edge ? 34 : 9;
				if constexpr (std::is_same_v<T, Wave>)
					return value.Distortion != 0 ? 1 + uint64_t(value.Detail) * 4 : 1;
				if constexpr (std::is_same_v<T, Blur>) return 2 * (2 * uint64_t(std::ceil(value.Radius)) + 1);
				if constexpr (std::is_same_v<T, NormalMap>) return 4;
				if constexpr (std::is_same_v<T, MeshAttribute>) return 1 + uint64_t(value.Padding);
				return 1;
			},
			operation
		);
	}
	// Generators and resampling operations declare their extent; the rest take the first input's.
	inline ImageExtent Extent(const Operation &operation, const ImageExtent &input, size_t bytesPerPixel) {
		return std::visit(
			[&](const auto &value) -> ImageExtent {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
							  std::is_same_v<T, Crop> || std::is_same_v<T, Transform> ||
							  std::is_same_v<T, Noise> || std::is_same_v<T, Voronoi> ||
							  std::is_same_v<T, Gradient> || std::is_same_v<T, Wave> ||
							  std::is_same_v<T, MeshAttribute>)
					return {value.Width, value.Height, size_t(value.Width) * value.Height * bytesPerPixel};
				else
					return {input.Width, input.Height, size_t(input.Width) * input.Height * bytesPerPixel};
			},
			operation
		);
	}
	// Whether every connected input must share the first input's canvas.
	inline bool MatchingInputs(const Operation &operation) {
		return std::holds_alternative<Blend>(operation) || std::holds_alternative<Math>(operation) ||
			   std::holds_alternative<Channels>(operation) || std::holds_alternative<Warp>(operation);
	}
}
