#include "../SourceSafeDraw.hpp"
#include "../TimelineDrivers.hpp"
#include "Curve.hpp"
#include "Gradient.hpp"
#include "Path.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace engine::imagegraph::detail {
	bool SourcePathBlur(NodeContext &context) {
		bool inactiveFailed = false;
		if (CopyWhenInactive(context, inactiveFailed)) return !inactiveFailed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Path Blur requires its input surface", "surface_in");
		const Value *pathValue = context.Find("blur_path");
		const Path2D *path = pathValue ? std::get_if<Path2D>(pathValue) : nullptr;
		const auto isDefault = context.IsCatalogueDefault("blur_path");
		if (!isDefault)
			return context.Fail(
				Status::UnsupportedExecution,
				"Path Blur requires resolved source default provenance",
				"blur_path"
			);
		if (*isDefault) {
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			return format && context.NewImage("surface_out", source->Width, source->Height, *format);
		}
		if (!path)
			return context.Fail(
				Status::InvalidValue, "Path Blur requires its selected path object", "blur_path"
			);
		if (source->Format == SurfaceFormat::R8Unorm || source->Format == SurfaceFormat::R16Float ||
			source->Format == SurfaceFormat::R32Float)
			return RunPixelProcessor(
				context, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
					return SourceSafeDrawPixel(surface, x, y);
				}
			);
		const int64_t mode = context.Integer("mode");
		if (mode < 0 || mode > 1)
			return context.Fail(Status::InvalidValue, "Path Blur mode is invalid", "mode");
		const double rawResolution = DriverRoundHalfEven(context.Scalar("resolution", 32));
		if (!std::isfinite(rawResolution))
			return context.Fail(Status::InvalidValue, "Path Blur resolution must be finite", "resolution");
		const uint32_t count = uint32_t(std::clamp(rawResolution, 2., 128.));
		const auto curveAt = [&](std::string_view port) -> const Curve * {
			const Value *value = context.Find(port);
			return value ? std::get_if<Curve>(value) : nullptr;
		};
		const Curve *scale = curveAt("scale_modulate"),
					*intensityCurve = mode == 0 ? curveAt("intensity_curve") : nullptr;
		const auto validCurve = [](const Curve *curve) {
			return curve && curve->Anchors.size() <= 9 && curve->Header[1] != 0;
		};
		if (!validCurve(scale))
			return context.Fail(
				Status::UnsupportedExecution,
				"Path Blur scale curve needs a defined bounded GLSL uniform",
				"scale_modulate"
			);
		if (mode == 0 && !validCurve(intensityCurve))
			return context.Fail(
				Status::UnsupportedExecution,
				"Path Blur intensity curve needs a defined bounded GLSL uniform",
				"intensity_curve"
			);
		const Value *colorValue = mode == 1 ? context.Find("color") : nullptr;
		const Gradient *color = colorValue ? std::get_if<Gradient>(colorValue) : nullptr;
		if (mode == 1 && (!color || color->Keys.empty() || color->Keys.size() > GRADIENT_KEY_SLOTS))
			return context.Fail(
				Status::UnsupportedExecution, "Path Blur blend requires a bounded source gradient", "color"
			);
		const uint64_t cost =
			5 + 8 * (scale->Anchors.size() + (intensityCurve ? intensityCurve->Anchors.size() : 0)) +
			(color ? color->Keys.size() : 0);
		if (context.ProcessorCount > 64000000 / (uint64_t(count) * cost) ||
			uint64_t(source->Width) * source->Height >
				64000000 / (uint64_t(count) * cost * std::max<size_t>(1, context.ProcessorCount)))
			return context.Fail(
				Status::LimitExceeded, "Path Blur samples and curves exceed work budget", "resolution"
			);
		PathRuntime runtime;
		if (!runtime.Init(context, *path)) return false;
		const PathPoint origin = runtime.PointRatio(context.Scalar("path_origin"));
		const Vector2 range = context.Vec2("range", {0, 1}), anchor = context.Vec2("anchor", {.5, .5}),
					  rotation = context.Vec2("rotation_modulate");
		std::array<Vector2, 128> points{};
		for (uint32_t i = 0; i < count; ++i) {
			const PathPoint point =
				runtime.PointRatio(std::clamp(range.X + (range.Y - range.X) * i / (count - 1), 0., .99));
			points[i] = {(point.X - origin.X) / source->Width, (point.Y - origin.Y) / source->Height};
			if (!std::isfinite(points[i].X) || !std::isfinite(points[i].Y))
				return context.Fail(
					Status::UnsupportedExecution, "Path Blur source path offset is nonfinite", "blur_path"
				);
		}
		const bool inverted = context.Boolean("inverted");
		const double intensity = mode == 0 ? context.Scalar("intensity", 1) : 1;
		const auto uv = ReadUvMap(context);
		SamplerSettings sampler = ReadSampler(context);
		sampler.Interpolation = Filtered(sampler) ? 2 : 1;
		GradientSampler gradient;
		if (color) gradient = ReadGradient(context, "color", *color);
		return RunPixelProcessor(context, [&](const Image &surface, uint32_t, uint32_t, double u, double v) {
			if (context.FailureCode != Status::Ok) return Rgba{};
			Rgba result{};
			double alphaWeight = 0;
			for (uint32_t step = 0; step < count; ++step) {
				const uint32_t index = inverted ? count - step - 1 : step;
				const double progress = double(index) / count;
				const double scaling = EvalShaderCurve(*scale, progress),
							 angle =
								 (rotation.X + (rotation.Y - rotation.X) * progress) * std::numbers::pi / 180;
				if (scaling == 0 || !std::isfinite(scaling) || !std::isfinite(angle)) {
					context.Fail(
						Status::UnsupportedExecution,
						"Path Blur source scale division or rotation is undefined",
						"scale_modulate"
					);
					return Rgba{};
				}
				const double dx = (u - points[index].X - anchor.X) / scaling,
							 dy = (v - points[index].Y - anchor.Y) / scaling;
				double sampleU = anchor.X + dx * std::cos(angle) - dy * std::sin(angle),
					   sampleV = anchor.Y + dx * std::sin(angle) + dy * std::cos(angle);
				// Source pointAmount equals resolution, so interpolation fraction is always0; no unused tail
				// is read.
				UvRemap(uv, sampleU, sampleV, progress, false);
				if (!std::isfinite(sampleU) || !std::isfinite(sampleV)) {
					context.Fail(
						Status::UnsupportedExecution,
						"Path Blur source sample coordinate is nonfinite",
						"blur_path"
					);
					return Rgba{};
				}
				Rgba sample = SampleTexture(surface, sampleU, sampleV, sampler);
				if (mode == 0) {
					const double amount = EvalShaderCurve(*intensityCurve, progress) * intensity;
					if (!std::isfinite(amount)) {
						context.Fail(
							Status::UnsupportedExecution,
							"Path Blur source intensity weighting is nonfinite",
							"intensity_curve"
						);
						return Rgba{};
					}
					for (size_t c = 0; c < 4; ++c) {
						sample[c] *= amount;
						result[c] += sample[c];
					}
					alphaWeight += sample[3];
				} else {
					if (sample[3] <= 0) continue;
					const Rgba tint = GradientEval(gradient, progress);
					for (double channel : tint)
						if (!std::isfinite(channel)) {
							context.Fail(
								Status::UnsupportedExecution,
								"Path Blur source gradient evaluation is undefined",
								"color"
							);
							return Rgba{};
						}
					Rgba foreground{};
					for (size_t c = 0; c < 4; ++c)
						foreground[c] = sample[c] * tint[c];
					const double alpha = foreground[3] + result[3] * (1 - foreground[3]);
					if (alpha == 0 || !std::isfinite(alpha)) {
						context.Fail(
							Status::UnsupportedExecution,
							"Path Blur source blend division is undefined",
							"color"
						);
						return Rgba{};
					}
					for (size_t c = 0; c < 3; ++c)
						result[c] =
							(foreground[c] * foreground[3] + result[c] * result[3] * (1 - foreground[3])) /
							alpha;
					result[3] = alpha;
				}
			}
			if (mode == 0) {
				if (alphaWeight == 0 || !std::isfinite(alphaWeight)) {
					context.Fail(
						Status::UnsupportedExecution,
						"Path Blur source blur alpha division is undefined",
						"intensity"
					);
					return Rgba{};
				}
				for (size_t c = 0; c < 3; ++c)
					result[c] /= alphaWeight;
				result[3] /= count;
			}
			return result;
		});
	}
}
