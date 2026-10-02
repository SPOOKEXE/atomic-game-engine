#include "Families.hpp"
#include "Processor.hpp"

#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		// Solid offers Pixel and Project units; its mask-size switch overrides the authored dimension.
		bool SolidDimension(NodeContext &context, const Image *mask, uint32_t &width, uint32_t &height) {
			if (mask && context.Boolean("use_mask_dimension", true)) {
				width = mask->Width;
				height = mask->Height;
				return true;
			}
			const int64_t unit = context.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(Status::InvalidValue, "dimension unit is invalid", "dimension_unit");
			return ResolveDimension(context, "dimension", width, height);
		}

		bool Solid(NodeContext &context) {
			const int64_t depth = context.Integer("attribute_color_depth", 1);
			std::optional<SurfaceFormat> format;
			if (depth == 1)
				format = context.InheritedSurfaceFormat;
			else if (depth == 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"Solid source input 0 is Dimension, so Input format has no surface",
					"attribute_color_depth"
				);
			else
				format = SourceSurfaceFormat(depth);
			if (!format)
				return context.Fail(
					depth < 0 || depth > 8 ? Status::InvalidValue : Status::UnsupportedExecution,
					"solid surface format cannot be resolved",
					"attribute_color_depth"
				);
			const Image *mask = context.Input("mask"), *foreground = context.Input("foreground");
			for (const auto &[port, image] : context.Images)
				if (!image ||
					!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
					!FiniteSurfaceSamples(*image))
					return context.Fail(Status::InvalidValue, "solid input image is invalid", port);
			uint32_t width = 0, height = 0;
			if (!SolidDimension(context, mask, width, height)) return false;
			Image *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			if (context.Boolean("empty")) {
				if (foreground)
					for (uint32_t y = 0; y < std::min(height, foreground->Height); y++)
						for (uint32_t x = 0; x < std::min(width, foreground->Width); x++)
							if (!WritePixel(*output, x, y, ReadPixel(*foreground, x, y)))
								return context.Fail(
									Status::InvalidValue,
									"solid sample exceeds output floating range",
									"foreground"
								);
				return true;
			}
			const Colour color = context.Get<Colour>("color", {255, 255, 255, 255});
			const Rgba base{color.Red / 255.0, color.Green / 255.0, color.Blue / 255.0, color.Alpha / 255.0};
			const bool maskAlphaOnly = context.Boolean("mask_alpha_only");
			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t x = 0; x < width; x++) {
					const double u = (x + .5) / width, v = (y + .5) / height;
					Rgba result = base;
					if (foreground) {
						const Rgba fg = SampleNearest(*foreground, u, v);
						for (size_t channel = 0; channel < 3; channel++)
							result[channel] = base[channel] * (1 - fg[3]) + fg[channel] * fg[3];
						result[3] = 1;
					}
					if (mask) {
						const Rgba sampled = SampleNearest(*mask, u, v);
						result[3] *= maskAlphaOnly ? sampled[3]
												   : (sampled[0] + sampled[1] + sampled[2]) / 3 * sampled[3];
					}
					if (!WritePixel(*output, x, y, result))
						return context.Fail(
							Status::InvalidValue, "solid sample exceeds output floating range", "surface_out"
						);
				}
			}
			return true;
		}
	}

	std::span<const ExecutorEntry> GenerateExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.solid", Solid, true}};
		return ENTRIES;
	}
}
