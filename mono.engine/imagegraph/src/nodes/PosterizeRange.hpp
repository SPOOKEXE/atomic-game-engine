#pragma once

#include "../SurfaceScratch.hpp"
#include "Processor.hpp"

#include <array>
#include <optional>

namespace engine::imagegraph::detail {
	// Pinned node_posterize and sh_get_max_downsampled: RGBA8 ping-pong surfaces, then byte extrema.
	inline bool PosterizeLocalRange(
		NodeContext &context,
		const Image &source,
		std::array<double, 3> &minimum,
		std::array<double, 3> &maximum
	) {
		std::array<std::optional<SurfaceScratch>, 4> temporary;
		for (auto &surface : temporary) {
			surface = MakeSurfaceScratch(
				context, source.Width, source.Height, SurfaceFormat::RGBA8Unorm, "surface_out"
			);
			if (!surface) return false;
		}
		if (!CopySurfaceSamples(context, source, temporary[0]->Data, "surface_out") ||
			!CopySurfaceSamples(context, source, temporary[1]->Data, "surface_out"))
			return false;
		// Nonpositive source repeat counts produce no reduction passes.
		uint64_t area = static_cast<uint64_t>(source.Width) * source.Height;
		uint32_t iterations = 0;
		while (area > 1024) {
			area = (area + 3) / 4;
			++iterations;
		}
		uint32_t width = (source.Width + 1) / 2, height = (source.Height + 1) / 2;
		size_t target = 1;
		for (uint32_t pass = 0; pass < iterations; ++pass) {
			// Replacement is admitted while the old target and both sampled surfaces still live.
			for (size_t output = 0; output < 2; ++output) {
				auto replacement =
					MakeSurfaceScratch(context, width, height, SurfaceFormat::RGBA8Unorm, "surface_out");
				if (!replacement) return false;
				temporary[target * 2 + output] = std::move(replacement);
			}
			const Image &previous = temporary[(1 - target) * 2]->Data;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					Rgba high{}, low{1, 1, 1, 1};
					for (uint32_t dy = 0; dy < 2; ++dy)
						for (uint32_t dx = 0; dx < 2; ++dx) {
							const Rgba sample =
								SampleNearest(previous, (x + 0.5 + dx) / width, (y + 0.5 + dy) / height);
							for (size_t channel = 0; channel < 4; ++channel) {
								high[channel] = std::max(high[channel], sample[channel]);
								// The pinned shader reads surfaceMax here too, despite its surfaceMin
								// uniform.
								low[channel] = std::min(low[channel], sample[channel]);
							}
						}
					// Source leaves normal blending active after resize. Native targets start clear;
					// source partial-alpha results also depend on unspecified recreated target contents.
					const double highAlpha = high[3], lowAlpha = low[3];
					for (size_t channel = 0; channel < 4; ++channel) {
						high[channel] *= highAlpha;
						low[channel] *= lowAlpha;
					}
					if (!WritePixel(temporary[target * 2]->Data, x, y, high) ||
						!WritePixel(temporary[target * 2 + 1]->Data, x, y, low))
						return context.Fail(
							Status::InvalidValue, "posterize reduction exceeds numeric range", "surface_out"
						);
				}
			width = (width + 1) / 2;
			height = (height + 1) / 2;
			target = 1 - target;
		}
		const Image &high = temporary[(1 - target) * 2]->Data;
		const Image &low = temporary[(1 - target) * 2 + 1]->Data;
		// CPU surfaces already own these bytes; scanning avoids duplicating GPU readback buffers.
		for (size_t pixel = 0; pixel < high.Pixels.size(); pixel += 4)
			for (size_t channel = 0; channel < 3; ++channel) {
				maximum[channel] = std::max(maximum[channel], double(high.Pixels[pixel + channel]));
				minimum[channel] = std::min(minimum[channel], double(low.Pixels[pixel + channel]));
			}
		return true;
	}
}
