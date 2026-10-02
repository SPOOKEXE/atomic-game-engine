#include "../SourceSafeDraw.hpp"
#include "ColorSpace.hpp"
#include "Curve.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	bool SourceGrain(NodeContext &context) {
		bool inactiveFailed = false;
		if (CopyWhenInactive(context, inactiveFailed)) return !inactiveFailed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Grain requires its input surface", "surface_in");
		const bool single = source->Format == SurfaceFormat::R8Unorm ||
							source->Format == SurfaceFormat::R16Float ||
							source->Format == SurfaceFormat::R32Float;
		if (single)
			return RunPixelProcessor(
				context, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
					return SourceSafeDrawPixel(surface, x, y);
				}
			);
		if (!context.Find("seed"))
			return context.Fail(Status::InvalidValue, "Grain requires its resolved source seed", "seed");
		const double seed = context.Scalar("seed");
		constexpr std::array<std::string_view, 7> controls{
			"brightness", "red", "green", "blue", "hue", "saturation", "value"
		};
		constexpr std::array<Vector2, 7> offsets{
			{{.156, .6169},
			 {.985, .3642},
			 {.653, .4954},
			 {.382, .2967},
			 {.685, .5672},
			 {.134, .8632},
			 {.268, .1264}}
		};
		std::array<const Curve *, 7> curves{};
		for (size_t i = 0; i < controls.size(); ++i) {
			const std::string id(controls[i]);
			if (context.Boolean(id + "_curved")) {
				const Value *value = context.Find(id + "_curve");
				curves[i] = value ? std::get_if<Curve>(value) : nullptr;
				if (!curves[i])
					return context.Fail(
						Status::InvalidValue, "Grain requires an enabled source curve", id + "_curve"
					);
				if (curves[i]->Anchors.size() > 9)
					return context.Fail(
						Status::UnsupportedExecution,
						"Grain GLSL curve uniform has only 64 slots; larger HLSL curves require a backend "
						"observation",
						id + "_curve"
					);
				if (curves[i]->Header[1] == 0)
					return context.Fail(
						Status::UnsupportedExecution,
						"Grain source curve input division is undefined",
						id + "_curve"
					);
			}
		}
		uint64_t cost = 7;
		for (const Curve *curve : curves)
			if (curve) cost += curve->Anchors.size();
		if (uint64_t(source->Width) * source->Height > 64000000 / cost)
			return context.Fail(
				Status::LimitExceeded, "Grain noise and curve scans exceed work budget", "surface_in"
			);
		struct Amount {
			double Scalar;
			const Image *Map;
			Vector2 Range;
		};
		std::array<Amount, 4> mapping{};
		for (size_t i = 0; i < mapping.size(); ++i) {
			const std::string id(controls[i]);
			mapping[i] = {
				context.Scalar(id),
				context.Boolean(id + "_mapped") ? context.Input(id + "_map") : nullptr,
				context.Vec2(id + "_map_range")
			};
		}
		const int64_t brightnessMode = context.Integer("blend_mode"),
					  rgbMode = context.Integer("blend_mode_2"), hsvMode = context.Integer("blend_mode_3");
		const auto blend = [](double base, double noise, double amount, int64_t mode) {
			if (mode == 0) return base + noise * amount;
			if (mode == 1) return base * (1 + noise * amount);
			const double result = mode == 2
									  ? 1 - (1 - base) * (1 - noise)
									  : (base < .5 ? 2 * base * noise : 1 - 2 * (1 - base) * (1 - noise));
			return base + (result - base) * amount;
		};
		return RunPixelProcessor(
			context, [&](const Image &surface, uint32_t x, uint32_t y, double u, double v) {
				Rgba colour = ReadPixel(surface, x, y);
				const double brightness = (colour[0] + colour[1] + colour[2]) / 3;
				std::array<double, 4> amounts{};
				std::array<double, 7> noise{};
				for (size_t i = 0; i < noise.size(); ++i) {
					const double phase =
						std::sin((u + offsets[i].X) * 12.9898 + (v + offsets[i].Y) * 78.233) * 43758.5453 +
						(seed - std::floor(seed / 100000) * 100000) / 10;
					const double n = phase - std::floor(phase);
					noise[i] = 1 / (.25 * std::sqrt(2 * 3.1415)) * std::exp(-n * n / (2 * .25 * .25));
					if (curves[i]) {
						const double value = EvalShaderCurve(*curves[i], brightness);
						if (!std::isfinite(value)) {
							context.Fail(
								Status::UnsupportedExecution,
								"Grain source curve evaluation is undefined",
								std::string(controls[i]) + "_curve"
							);
							return Rgba{};
						}
						noise[i] *= value;
					}
					if (i < amounts.size()) {
						double amount = mapping[i].Scalar;
						if (mapping[i].Map) {
							const Rgba texel = SampleNearest(*mapping[i].Map, u, v);
							amount = mapping[i].Range.X + (mapping[i].Range.Y - mapping[i].Range.X) *
															  (texel[0] + texel[1] + texel[2]) / 3;
						}
						amounts[i] = amount * amount * amount;
					}
				}
				for (size_t i = 0; i < 3; ++i)
					colour[i] = blend(colour[i], noise[0], amounts[0], brightnessMode);
				for (size_t i = 0; i < 3; ++i)
					colour[i] = blend(colour[i], noise[i + 1], amounts[i + 1], rgbMode);
				Rgb3 hsv = ShaderRgbToHsv({colour[0], colour[1], colour[2]});
				// sh_grain applies RGB amounts again in HSV; its authored HSV amounts are
				// unused.
				for (size_t i = 0; i < 3; ++i)
					hsv[i] = blend(hsv[i], noise[i + 4], amounts[i + 1], hsvMode);
				const Rgb3 rgb = ShaderHsvToRgb(hsv);
				for (size_t i = 0; i < 3; ++i)
					colour[i] = rgb[i];
				return colour;
			}
		);
	}
} // namespace engine::imagegraph::detail
