#pragma once
#include "FluidPayload.hpp"
#include "nodes/Sampler.hpp"

#include <bit>
namespace engine::imagegraph::detail {
	inline int32_t SourceFlipSpriteInt32(double value) {
		if (!std::isfinite(value)) return 0;
		double wrapped = std::fmod(std::trunc(value), 4294967296.);
		if (wrapped < 0) wrapped += 4294967296.;
		return std::bit_cast<int32_t>(uint32_t(wrapped));
	}
	inline double SourceFlipSpriteVertexAlpha(double alpha) {
		// Preserve the official HTML5 signed32 bitwise clamp, including overflow edges.
		int64_t a = SourceFlipSpriteInt32(alpha * 255);
		a -= SourceFlipSpriteInt32(double(a) - 255) & (SourceFlipSpriteInt32(255 - double(a)) >> 31);
		a -= SourceFlipSpriteInt32(double(a)) & (SourceFlipSpriteInt32(double(a)) >> 31);
		return (uint32_t(SourceFlipSpriteInt32(double(a))) & 255) / 255.;
	}

	// Surface arrays use the first frame's half dimensions for all frame origins.
	// Geometry follows official HTML5 float32 vertices, with CPU point/bilinear
	// sample and top-left rectangle coverage; captured GPU parity remains separate.
	template <class Blend>
	bool DrawSourceFlipSprite(
		NodeContext &context,
		Image &output,
		const Image &sprite,
		Vector2 firstSize,
		Vector2 position,
		double ratio,
		Colour color,
		double opacity,
		bool filtered,
		uint64_t &work,
		Blend blend
	) {
		const double left = double(float(position.X - firstSize.X * .5 * ratio)),
					 top = double(float(position.Y - firstSize.Y * .5 * ratio)),
					 right = double(float(position.X - firstSize.X * .5 * ratio + sprite.Width * ratio)),
					 bottom = double(float(position.Y - firstSize.Y * .5 * ratio + sprite.Height * ratio));
		if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom))
			return context.Fail(
				Status::InvalidValue, "FLIP sprite float32 geometry is undefined", "fluid_particle"
			);
		if (left == right || top == bottom) return true;
		const auto x0 = uint32_t(std::clamp(std::ceil(std::min(left, right) - .5), 0., double(output.Width))),
				   x1 = uint32_t(std::clamp(std::ceil(std::max(left, right) - .5), 0., double(output.Width))),
				   y0 =
					   uint32_t(std::clamp(std::ceil(std::min(top, bottom) - .5), 0., double(output.Height))),
				   y1 =
					   uint32_t(std::clamp(std::ceil(std::max(top, bottom) - .5), 0., double(output.Height)));
		const uint64_t samples = uint64_t(x1 - x0) * (y1 - y0);
		if (samples > FluidDomainLimits::MaximumWork - work)
			return context.Fail(
				Status::LimitExceeded, "FLIP sprite raster exceeds bounded work", "fluid_particle"
			);
		work += samples;
		const double alpha = SourceFlipSpriteVertexAlpha(opacity * ratio);
		for (uint32_t y = y0; y < y1; ++y)
			for (uint32_t x = x0; x < x1; ++x) {
				auto pixel = Texture(
					sprite, (x + .5 - left) / (right - left), (y + .5 - top) / (bottom - top), filtered
				);
				pixel[0] *= color.Red / 255.;
				pixel[1] *= color.Green / 255.;
				pixel[2] *= color.Blue / 255.;
				pixel[3] *= alpha;
				if (!blend(x, y, pixel)) return false;
			}
		return true;
	}
}
