#include "../SourceSafeDraw.hpp"
#include "Curve.hpp"
#include "Gradient.hpp"
#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	bool SourceRadialBlur(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Radial Blur requires its input surface", "surface_in");
		if (source->Format == SurfaceFormat::R8Unorm || source->Format == SurfaceFormat::R16Float ||
			source->Format == SurfaceFormat::R32Float)
			return RunPixelProcessor(context, [](const Image &image, uint32_t x, uint32_t y, double, double) {
				return SourceSafeDrawPixel(image, x, y);
			});
		const int64_t colorize = context.Integer("colorize");
		if (colorize < 0 || colorize > 2)
			return context.Fail(Status::InvalidValue, "Radial Blur colorize mode is invalid", "colorize");
		const auto sampler = ReadSampler(context);
		if (!SupportedSampler(context, sampler)) return false;
		const auto uv = ReadUvMap(context);
		const double scalar = context.Scalar("strength", 45);
		const bool mapped = context.Boolean("strength_mapped"), curved = context.Boolean("strength_curved"),
				   fadeDistance = context.Boolean("fade_distance"),
				   gamma = context.Boolean("gamma_correction");
		const Image *map = mapped ? context.Input("strength_map") : nullptr;
		const Vector2 range = mapped ? context.Vec2("strength_map_range", {0, 45}) : Vector2{scalar, scalar};
		// The pinned shader takes max before abs, truncating negative mapped ranges literally.
		const double maximum = std::abs(std::max(range.X, range.Y));
		if (!std::isfinite(maximum) || maximum > 32000000)
			return context.Fail(
				Status::LimitExceeded, "Radial Blur tap range exceeds work budget", "strength"
			);
		const uint64_t taps = uint64_t(std::floor(maximum * 2)) + 1;
		const Value *curveValue = curved ? context.Find("strength_curve") : nullptr;
		const Curve *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (curved && (!curve || curve->Anchors.size() > 9 || curve->Header[1] == 0))
			return context.Fail(
				Status::UnsupportedExecution,
				"Radial Blur enabled curve needs a bounded defined GLSL uniform",
				"strength_curve"
			);
		const Value *gradientValue = colorize == 2 ? context.Find("gradient") : nullptr;
		const Gradient *keys = gradientValue ? std::get_if<Gradient>(gradientValue) : nullptr;
		if (colorize == 2 && (!keys || keys->Keys.empty() || keys->Keys.size() > GRADIENT_KEY_SLOTS))
			return context.Fail(
				Status::InvalidValue, "Radial Blur requires a bounded spectral gradient", "gradient"
			);
		GradientSampler gradient;
		if (keys) gradient = ReadGradient(context, "gradient", *keys);
		const uint64_t curveCost = (sampler.Interpolation == 4 ? 36
									: Filtered(sampler)		   ? 4
															   : 1) +
								   (curve ? curve->Anchors.size() : 0) + (keys ? keys->Keys.size() : 0);
		if (taps > 64000000 / curveCost ||
			uint64_t(source->Width) * source->Height > 64000000 / (taps * curveCost))
			return context.Fail(
				Status::LimitExceeded, "Radial Blur taps and curves exceed work budget", "strength"
			);
		Vector2 center = context.Vec2("center", {.5, .5});
		if (!context.IsLinked("center") && context.Integer("center_unit", 1) == 1) {
			center.X *= source->Width;
			center.Y *= source->Height;
		}
		const double intensity = context.Scalar("intensity", 1), scale = context.Scalar("scale", 1),
					 shift = context.Scalar("shift");
		constexpr std::array<double, 3> c1{3.54585104, 2.93225262, 2.41593945},
			x1{.69549072, .49228336, .27699880}, y1{.02312639, .15225084, .52607955},
			c2{3.90307140, 3.21182957, 3.96587128}, x2{.11748627, .86755042, .66077860},
			y2{.84897130, .88445281, .73949448};
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			if (context.FailureCode != Status::Ok) return Rgba{};
			double strength = range.X;
			if (map) {
				const Rgba pixel = Texture(*map, u, v, false);
				strength = range.X + (range.Y - range.X) * (pixel[0] + pixel[1] + pixel[2]) / 3;
			}
			strength = std::abs(strength);
			const Vector2 delta{u * image.Width - center.X, v * image.Height - center.Y};
			if (delta.X == 0 && delta.Y == 0) {
				context.Fail(
					Status::UnsupportedExecution,
					"Radial Blur source atan is undefined at its center",
					"center"
				);
				return Rgba{};
			}
			const double angle = std::atan2(delta.Y, delta.X), distance = std::hypot(delta.X, delta.Y);
			Rgba result{};
			Rgb3 spectral{};
			double weight = 0, alphaWeight = 0;
			for (uint64_t step = 0; step < taps; ++step) {
				const double i = -maximum + double(step);
				if (i < -strength) continue;
				if (i > strength) break;
				if (strength == 0 && (uv.Map || curved || fadeDistance || colorize != 0)) {
					context.Fail(
						Status::UnsupportedExecution,
						"Radial Blur source strength division is undefined",
						"strength"
					);
					return Rgba{};
				}
				const double influence = strength == 0 ? 0 : std::abs(i) / strength;
				const double rotation = angle + i / 100;
				double sampleU = (center.X + std::cos(rotation) * distance) / image.Width;
				double sampleV = (center.Y + std::sin(rotation) * distance) / image.Height;
				UvRemap(uv, sampleU, sampleV, influence, false);
				Rgba colour = SampleTexture(image, sampleU, sampleV, sampler);
				double fade = fadeDistance ? 1 - influence : 1;
				if (curve) fade *= EvalShaderCurve(*curve, 1 - influence);
				for (size_t channel = 0; channel < 3; ++channel) {
					if (gamma) {
						if (colour[channel] < 0) {
							context.Fail(
								Status::UnsupportedExecution,
								"Radial Blur source gamma power is undefined for negative samples",
								"surface_in"
							);
							return Rgba{};
						}
						colour[channel] = std::pow(colour[channel], 2.2);
					}
					colour[channel] *= fade;
				}
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] += colour[channel];
				weight += colour[3] * fade;
				alphaWeight += fade;
				if (colorize) {
					const double offset = ShaderFract(std::abs(influence * scale) + shift),
								 power =
									 std::sqrt(
										 colour[0] * colour[0] + colour[1] * colour[1] + colour[2] * colour[2]
									 ) *
									 colour[3] * intensity;
					Rgba tint{};
					if (colorize == 2) tint = GradientEval(gradient, offset);
					for (size_t channel = 0; channel < 3; ++channel) {
						const double a = c1[channel] * (offset - x1[channel]),
									 b = c2[channel] * (offset - x2[channel]);
						const double spectralColour = colorize == 2
														  ? tint[channel]
														  : std::clamp(1 - a * a - y1[channel], 0., 1.) +
																std::clamp(1 - b * b - y2[channel], 0., 1.);
						spectral[channel] += spectralColour * power;
					}
				}
			}
			if (weight == 0 || alphaWeight == 0) {
				context.Fail(
					Status::UnsupportedExecution,
					"Radial Blur source accumulation division is undefined",
					"surface_in"
				);
				return Rgba{};
			}
			for (size_t channel = 0; channel < 3; ++channel) {
				result[channel] = (result[channel] + spectral[channel]) / weight;
				if (gamma) {
					if (result[channel] < 0) {
						context.Fail(
							Status::UnsupportedExecution,
							"Radial Blur source inverse gamma power is undefined",
							"surface_in"
						);
						return Rgba{};
					}
					result[channel] = std::pow(result[channel], 1 / 2.2);
				}
			}
			result[3] /= alphaWeight;
			return result;
		});
	}
}
