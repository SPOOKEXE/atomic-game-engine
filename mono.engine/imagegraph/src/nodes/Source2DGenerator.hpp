#pragma once
#include "Source2DMath.hpp"

namespace engine::imagegraph::detail::source2d {
	inline Vector2
	GeneratorUv(const NodeContext &context, double u, double v, double &alpha, bool filtered = false) {
		alpha = 1;
		const Image *map = context.Input("uv_map");
		if (!map) return {u, v};
		const Rgba pixel = filtered ? BilinearClamp(*map, u, v) : SampleNearest(*map, u, v);
		const double amount = context.Scalar("uv_mix", 1);
		alpha = pixel[3];
		return {u + (pixel[0] - u) * amount, v + (1.0 - pixel[1] - v) * amount};
	}

	template <class Shade> bool RunGenerator(NodeContext &context, Shade &&shade) {
		uint32_t width = 0, height = 0;
		const Image *mask = context.Input("mask");
		if (context.Integer("dimension_unit", 1) == 2) {
			if (!mask) return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
			width = mask->Width;
			height = mask->Height;
		} else if (!ResolveDimension(context, "dimension", width, height))
			return false;
		const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + 0.5) / width, v = (y + 0.5) / height;
				const Rgba generated = shade(width, height, x, y, u, v);
				if (context.FailureCode != Status::Ok) return false;
				if (!WritePixel(*output, x, y, generated))
					return context.Fail(
						Status::InvalidValue, "Generator sample exceeds surface range", "surface_out"
					);
				if (mask) {
					Rgba colour = ReadPixel(*output, x, y);
					const Rgba sample = SampleNearest(*mask, u, v);
					colour[3] *= (sample[0] + sample[1] + sample[2]) / 3.0 * sample[3];
					for (double &channel : colour)
						channel = Quantize(channel) / 255.0;
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "Masked generator sample exceeds surface range", "mask"
						);
				}
			}
		return context.FailureCode == Status::Ok;
	}

	inline Rgba InputColour(const NodeContext &context, std::string_view port, Colour fallback = {}) {
		const Colour colour = context.Get<Colour>(port, fallback);
		return {colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0};
	}
}
