#include "../PixelOpsCurveColor.hpp"
#include "Processor.hpp"

#include <numeric>

namespace engine::imagegraph::detail {
	bool PixelShine(NodeContext &context) {
		const Image *surface = context.Input("surface"), *mask = context.Input("mask"),
					*offset = context.Input("offset"), *uvMap = context.Input("uv_map");
		const uint32_t width = surface ? surface->Width : context.Project.SurfaceWidth;
		const uint32_t height = surface ? surface->Height : context.Project.SurfaceHeight;
		std::vector<double> shines{2, 1, 1};
		auto controlsCharge = context.ReserveWorkspace(64 * sizeof(double) + 256 * sizeof(Rgba));
		if (!controlsCharge) return false;
		if (const Value *value = context.Find("shines")) {
			shines.clear();
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Nested.empty() || !array->Items.empty() || array->Elements.size() > 64)
					return context.Fail(
						Status::InvalidValue, "Shines require a bounded numeric array", "shines"
					);
				shines.reserve(array->Elements.size());
				for (const auto &item : array->Elements) {
					if (const auto *number = std::get_if<double>(&item))
						shines.push_back(*number);
					else if (const auto *number = std::get_if<int64_t>(&item))
						shines.push_back(double(*number));
					else
						return context.Fail(
							Status::InvalidValue, "Shines require numeric segments", "shines"
						);
				}
			} else if (const auto *vector = std::get_if<Vector3>(value))
				shines = {vector->X, vector->Y, vector->Z};
			else
				return context.Fail(Status::InvalidValue, "Shines require a numeric array", "shines");
		}
		std::vector<Rgba> colors;
		const auto appendColor = [&](Colour color) {
			colors.push_back(
				{color.Red / 255.0, color.Green / 255.0, color.Blue / 255.0, color.Alpha / 255.0}
			);
		};
		if (const Value *value = context.Find("colors")) {
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array || !array->Nested.empty() || !array->Items.empty() || array->Elements.empty() ||
				array->Elements.size() > 256)
				return context.Fail(
					Status::InvalidValue, "Shine palette requires one to 256 colors", "colors"
				);
			colors.reserve(array->Elements.size());
			for (const auto &item : array->Elements) {
				if (const auto *color = std::get_if<Colour>(&item))
					appendColor(*color);
				else
					return context.Fail(Status::InvalidValue, "Shine palette requires colors", "colors");
			}
		} else
			appendColor({255, 255, 255, 255});
		const bool axis = context.Integer("axis") == 1, flip = context.Boolean("flip"),
				   keepAlpha = context.Boolean("keep_alpha"), alphaOnly = context.Boolean("mask_alpha_only"),
				   curved = context.Boolean("slope_curved");
		const double progress = context.Scalar("progress", .5), scale = context.Scalar("scale", 1),
					 slope = context.Scalar("slope", 1), intensity = context.Scalar("intensity", 1),
					 uvMix = context.Scalar("uv_mix", 1);
		const int64_t blend = context.Integer("blend_mode");
		const Vector2 range = context.Vec2("range", {0, .1});
		const auto *curveValue = context.Find("slope_curve");
		const Curve *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (curved && (!curve || !ValidColorCurve(*curve)))
			return context.Fail(
				Status::InvalidValue, "Curved shine slope requires a source curve", "slope_curve"
			);
		if (uint64_t(width) * height > 64000000 / (shines.size() + 1))
			return context.Fail(Status::LimitExceeded, "Shine sampling exceeds work budget");
		const double shineWidth = std::accumulate(shines.begin(), shines.end(), 0.0) * scale;
		Image *output = context.NewImage("surface_out", width, height, SurfaceFormat::RGBA8Unorm);
		Image *shineMask = context.NewImage("mask", width, height, SurfaceFormat::RGBA8Unorm);
		if (!output || !shineMask || context.FailureCode != Status::Ok) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				const Rgba original = surface ? SampleNearest(*surface, u, v) : Rgba{};
				Rgba result = original, shine{};
				double amount = intensity, position = progress, alpha = 1;
				Vector2 uv{u, v};
				if (mask) {
					const auto sample = SampleNearest(*mask, u, v);
					amount *= alphaOnly ? sample[3] : (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
				}
				if (offset) {
					const auto sample = SampleNearest(*offset, u, v);
					position +=
						range.X + (range.Y - range.X) * (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
				}
				if (uvMap) {
					const auto sample = SampleNearest(*uvMap, u, v);
					uv = {u + (sample[0] - u) * uvMix, v + (1 - sample[1] - v) * uvMix};
					alpha = sample[3];
				}
				amount *= alpha;
				Vector2 pixel{uv.X * width, uv.Y * height};
				if (axis) std::swap(pixel.X, pixel.Y);
				const double w = axis ? height : width, h = axis ? width : height;
				double start;
				if (slope == 0)
					start = flip ? w + shineWidth - (w + 2 * shineWidth) * position
								 : -shineWidth + (w + 2 * shineWidth) * position;
				else {
					double localSlope = slope;
					if (curved) {
						double factor = 0;
						if (SampleColorCurveUnchecked(*curve, pixel.Y / h, factor) != CurveColorStatus::Ok)
							return context.Fail(
								Status::InvalidValue, "Shine slope curve is undefined", "slope_curve"
							);
						localSlope *= factor;
					}
					if (localSlope == 0)
						return context.Fail(Status::InvalidValue, "Shine slope divides by zero", "slope");
					const double total = w + shineWidth;
					start = -w - total + (2 * w + 2 * total) * position;
					start += flip ? pixel.Y / localSlope : w - pixel.Y / localSlope;
				}
				bool fill = true;
				size_t fillIndex = 0;
				for (double segment : shines) {
					const double end = start + segment * scale;
					if (fill) ++fillIndex;
					if (fill && pixel.X > start && pixel.X <= end) {
						const auto &color = colors[colors.size() - fillIndex % colors.size() - 1];
						shine = color;
						for (size_t channel = 0; channel < 4; ++channel) {
							const double target = blend == 0   ? color[channel]
												  : blend == 1 ? original[channel] + color[channel]
															   : original[channel] * color[channel];
							result[channel] += (target - result[channel]) * amount;
						}
						break;
					}
					fill = !fill;
					start = end;
				}
				if (surface && original[3] == 0)
					result = {};
				else if (keepAlpha)
					result[3] = original[3];
				if (!WritePixel(*output, x, y, result) || !WritePixel(*shineMask, x, y, shine))
					return context.Fail(Status::InvalidValue, "Shine sample is nonfinite");
			}
		return context.FailureCode == Status::Ok;
	}
}
