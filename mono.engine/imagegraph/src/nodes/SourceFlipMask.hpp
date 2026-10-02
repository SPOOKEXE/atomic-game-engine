#pragma once
#include "FluidPayload.hpp"
#include "Sampler.hpp"

#include <engine/imagegraph/FluidDomain.hpp>
namespace engine::imagegraph::detail {
	// Caller admits two grid-sized R8 surfaces before this source shader reference
	// allocates them.
	inline bool BuildSourceFlipMask(
		NodeContext &context,
		const Image &input,
		uint32_t width,
		uint32_t height,
		double threshold,
		double expands,
		Image &mask,
		std::string_view port = "collider"
	) {
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP surface-mask CPU reference requires captured GPU "
				"filtering/readback coverage",
				port
			);
		const uint64_t cells = uint64_t(width) * height;
		if (!width || !height || cells > FluidDomainLimits::MaximumCells || !std::isfinite(threshold) ||
			!std::isfinite(expands) || expands > FluidDomainLimits::MaximumIterations)
			return context.Fail(
				Status::InvalidValue, "FLIP surface mask controls exceed the native finite bounds"
			);
		const uint64_t rings = expands > 0 ? uint64_t(std::floor(expands)) : 0;
		if (rings && cells > FluidDomainLimits::MaximumWork / (rings * 64))
			return context.Fail(
				Status::LimitExceeded, "FLIP surface mask dilation exceeds bounded sample work"
			);
		Image first;
		first.Width = width;
		first.Height = height;
		first.Format = SurfaceFormat::R8Unorm;
		first.Pixels.resize(cells);
		mask.Width = width;
		mask.Height = height;
		mask.Format = SurfaceFormat::R8Unorm;
		mask.Pixels.resize(cells);
		const bool filtered = Filtered(ReadSampler(context));
		const float shaderThreshold = float(threshold);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const auto sample =
					Texture(input, (double(x) + .5) / width, (double(y) + .5) / height, filtered);
				const float weight =
					float(float(float(float(sample[0]) + float(sample[1])) + float(sample[2])) / .3f) *
					float(sample[3]);
				if (std::isnan(weight))
					return context.Fail(
						Status::InvalidValue, "FLIP surface threshold produced undefined source weight"
					);
				first.Pixels[size_t(y) * width + x] = weight >= shaderThreshold ? 255 : 0;
			}
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				// surface_reset_shader disables filtering before this second pass.
				double weight = first.Pixels[size_t(y) * width + x] / 255.;
				if (weight != 1) {
					bool found = false;
					for (uint64_t ring = 1; ring <= rings && !found; ++ring)
						for (size_t angle = 0; angle < 64; ++angle) {
							const double radians = double(angle) / 64 * std::numbers::pi * 2;
							weight = Texture(
								first,
								(double(x) + .5 + std::cos(radians) * ring) / width,
								(double(y) + .5 + std::sin(radians) * ring) / height,
								false
							)[0];
							if (weight > 0) {
								found = true;
								break;
							}
						}
				}
				if (!StoreSurfacePixel(mask, x, y, {weight, 0, 0, 1}))
					return context.Fail(
						Status::InvalidValue, "FLIP surface expansion produced nonfinite source weight"
					);
			}
		return true;
	}
	inline void
	ApplySourceFlipSolidMask(FluidDomainData &domain, const Image &mask, uint32_t cellsX, uint32_t cellsY) {
		auto &solid = domain.Buffers[size_t(FluidBuffer::Solid)];
		for (uint32_t y = 1; y + 1 < cellsY; ++y)
			for (uint32_t x = 1; x + 1 < cellsX; ++x)
				if (mask.Pixels[size_t(y) * cellsX + x] > 128) solid[size_t(x) * cellsY + y] = 1;
	}
} // namespace engine::imagegraph::detail
