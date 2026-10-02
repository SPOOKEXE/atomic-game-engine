#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace studio::detail {
	inline bool ImageGraphValuePreviewSupported(engine::imagegraph::ValueType type) {
		using engine::imagegraph::ValueType;
		return engine::imagegraph::IsAuthoredValueType(type) || type == ValueType::Any ||
			   type == ValueType::Struct || type == ValueType::Path3D || type == ValueType::PixelBox;
	}
	// Builds the bounded RGBA8 display payload without changing the cached source
	// image.
	bool PrepareImageGraphPreviewRgba8(
		const engine::imagegraph::Image &image, std::vector<std::byte> &rgba8
	) noexcept;
} // namespace studio::detail
