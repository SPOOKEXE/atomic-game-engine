#pragma once

#include <engine/imagegraph/Surface.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace studio::detail {
	// Builds the bounded RGBA8 display payload without changing the cached source
	// image.
	bool PrepareImageGraphPreviewRgba8(
		const engine::imagegraph::Image &image, std::vector<std::byte> &rgba8
	) noexcept;
} // namespace studio::detail
