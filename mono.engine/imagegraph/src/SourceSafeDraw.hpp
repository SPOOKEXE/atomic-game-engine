#pragma once
#include "nodes/Processor.hpp"

namespace engine::imagegraph::detail {
	// __channel_pre selects sh_draw_r8/r16/r32, which replicate red into RGB before tinting.
	inline Rgba SourceSafeDrawPixel(const Image &surface, uint32_t x, uint32_t y) {
		Rgba colour = ReadPixel(surface, x, y);
		if (surface.Format == SurfaceFormat::R8Unorm || surface.Format == SurfaceFormat::R16Float ||
			surface.Format == SurfaceFormat::R32Float)
			colour = {colour[0], colour[0], colour[0], 1};
		return colour;
	}
}
