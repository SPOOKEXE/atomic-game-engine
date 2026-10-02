#include "../SourceSafeDraw.hpp"
#include "../TimelineDrivers.hpp"
#include "Curve.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace engine::imagegraph::detail {
	bool SourceBokehBlur(NodeContext &context) {
		bool inactiveFailed = false;
		if (CopyWhenInactive(context, inactiveFailed)) return !inactiveFailed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Lens Blur requires its input surface", "surface_in");
		if (source->Format == SurfaceFormat::R8Unorm || source->Format == SurfaceFormat::R16Float ||
			source->Format == SurfaceFormat::R32Float)
			return RunPixelProcessor(
				context, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
					return SourceSafeDrawPixel(surface, x, y);
				}
			);
		// Source Int valueProcess rounds each scalar or depth-one array leaf before shader submission.
		const double iteration = DriverRoundHalfEven(context.Scalar("iteration", 512));
		if (!std::isfinite(iteration) || iteration > 64000000)
			return context.Fail(
				Status::LimitExceeded, "Lens Blur iteration exceeds work budget", "iteration"
			);
		if (iteration <= 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"Lens Blur empty source accumulation divides by zero",
				"iteration"
			);
		const uint64_t taps = uint64_t(std::ceil(iteration));
		const bool mapped = context.Boolean("strength_mapped"), curved = context.Boolean("strength_curved");
		const double scalar = context.Scalar("strength", .2);
		const Vector2 range = mapped ? context.Vec2("strength_map_range", {0, .2}) : Vector2{scalar, scalar};
		const Image *map = mapped ? context.Input("strength_map") : nullptr;
		const Value *curveValue = curved ? context.Find("strength_curve") : nullptr;
		const Curve *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (curved && (!curve || curve->Anchors.size() > 9 || curve->Header[1] == 0))
			return context.Fail(
				Status::UnsupportedExecution,
				"Lens Blur enabled curve needs a bounded defined GLSL uniform",
				"strength_curve"
			);
		const SamplerSettings hardware = ReadSampler(context);
		const bool filtered = Filtered(hardware);
		const uint64_t tapCost = (filtered ? 4 : 1) + (curve ? 8 * curve->Anchors.size() : 0) + 1;
		const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		if (taps > 64000000 / tapCost || rows > 64000000 / (taps * tapCost) ||
			uint64_t(source->Width) * source->Height > 64000000 / (taps * tapCost * rows))
			return context.Fail(
				Status::LimitExceeded, "Lens Blur samples and curves exceed work budget", "iteration"
			);
		SamplerSettings sampler = hardware;
		// sampler_simple calls plain texture2D; only its hardware filtering state is consumed.
		sampler.Interpolation = filtered ? 2 : 1;
		const auto uv = ReadUvMap(context);
		const double contrast = context.Scalar("contrast", 150),
					 factor = std::max(context.Scalar("contrast_factor", 9), 1.),
					 smoothness = context.Scalar("smoothness", 2);
		const double angle = 2.39996323 *
							 (1 + (context.Scalar("rotation", 1) - 1) * std::numbers::pi / 180 / 100),
					 cosine = std::cos(angle), sine = std::sin(angle);
		if (!std::isfinite(angle))
			return context.Fail(
				Status::UnsupportedExecution, "Lens Blur source rotation is nonfinite", "rotation"
			);
		return RunPixelProcessor(context, [&](const Image &surface, uint32_t, uint32_t, double u, double v) {
			if (context.FailureCode != Status::Ok) return Rgba{};
			double strength = range.X;
			if (map) {
				const Rgba pixel = Texture(*map, u, v, false);
				strength = range.X + (range.Y - range.X) * (pixel[0] + pixel[1] + pixel[2]) / 3;
			}
			double reciprocal = 1;
			Vector2 hang{0, strength * .01 / std::sqrt(iteration)};
			Rgba result{};
			std::array<double, 3> weights{};
			for (uint64_t step = 0; step < taps; ++step) {
				reciprocal += 1 / reciprocal;
				// GLSL row-vector multiplication hang * rot preserves this rotation direction.
				hang = {hang.X * cosine + hang.Y * sine, -hang.X * sine + hang.Y * cosine};
				const double ratio = double(step) / iteration;
				double sampleU = u + double(surface.Height) / surface.Width * (reciprocal - 1) * hang.X,
					   sampleV = v + (reciprocal - 1) * hang.Y;
				UvRemap(uv, sampleU, sampleV, ratio, false);
				if (!std::isfinite(sampleU) || !std::isfinite(sampleV)) {
					context.Fail(
						Status::UnsupportedExecution,
						"Lens Blur source sampling coordinate is nonfinite",
						"strength"
					);
					return Rgba{};
				}
				const Rgba sample = SampleTexture(surface, sampleU, sampleV, sampler);
				const double coefficient = curve ? EvalShaderCurve(*curve, ratio) : 1;
				double alphaWeight = 0;
				for (size_t channel = 0; channel < 3; ++channel) {
					const double color = sample[channel] * sample[3];
					if (color < 0) {
						context.Fail(
							Status::UnsupportedExecution,
							"Lens Blur source power is undefined for negative premultiplied samples",
							"surface_in"
						);
						return Rgba{};
					}
					const double bokeh = (smoothness + std::pow(color, factor) * contrast) * coefficient *
										 coefficient * coefficient;
					if (!std::isfinite(bokeh)) {
						context.Fail(
							Status::UnsupportedExecution,
							"Lens Blur source weighting is nonfinite",
							"contrast"
						);
						return Rgba{};
					}
					result[channel] += color * bokeh;
					weights[channel] += bokeh;
					alphaWeight += bokeh;
				}
				result[3] += sample[3] * alphaWeight / 3;
			}
			const double alphaDivisor = (weights[0] + weights[1] + weights[2]) / 3;
			if (weights[0] == 0 || weights[1] == 0 || weights[2] == 0 || alphaDivisor == 0) {
				context.Fail(
					Status::UnsupportedExecution,
					"Lens Blur source accumulation division is undefined",
					"smoothness"
				);
				return Rgba{};
			}
			for (size_t channel = 0; channel < 3; ++channel)
				result[channel] /= weights[channel];
			result[3] /= alphaDivisor;
			// Spectral/gradient uniforms are submitted but unused by the pinned shader; spec remains zero.
			return result;
		});
	}
}
