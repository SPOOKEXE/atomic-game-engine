#include "../SourceSafeDraw.hpp"
#include "Gradient.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::imagegraph::detail {
	bool SourceSimpleBlur(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Non-Uniform Blur requires a surface", "surface_in");
		if (source->Format == SurfaceFormat::R8Unorm || source->Format == SurfaceFormat::R16Float ||
			source->Format == SurfaceFormat::R32Float)
			return RunPixelProcessor(
				context, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
					return SourceSafeDrawPixel(surface, x, y);
				}
			);
		const double shift = context.Scalar("shift");
		// Gradient.addShift replaces physical input17; both shader UV and gradient shift read that scalar.
		if (context.Input("uv_map"))
			return context.Fail(
				Status::UnsupportedExecution,
				"Non-Uniform Blur physical input17 is Shift, not an independent UV surface",
				"uv_map"
			);
		if (!std::isfinite(shift) || shift > 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"Positive source Shift needs an observed surface-handle alias for its UV lookup",
				"shift"
			);
		const double size = context.Scalar("size", 8) * UnitScale(context, "size", source->Width);
		if (!std::isfinite(size))
			return context.Fail(Status::InvalidValue, "Non-Uniform Blur size must be finite", "size");
		const Image *mask = context.Input("blur_mask");
		double maximumMask = 1;
		uint64_t maskPixels = 0;
		if (mask) {
			maskPixels = uint64_t(mask->Width) * mask->Height;
			if (maskPixels > 64000000 / std::max(uint64_t{1}, uint64_t(context.ProcessorCount)))
				return context.Fail(
					Status::LimitExceeded, "Non-Uniform Blur mask scan exceeds batch work budget", "blur_mask"
				);
			double rgbBound = 0, alphaBound = 0;
			for (uint32_t y = 0; y < mask->Height; ++y)
				for (uint32_t x = 0; x < mask->Width; ++x) {
					const Rgba pixel = ReadPixel(*mask, x, y);
					rgbBound = std::max(
						rgbBound, (std::abs(pixel[0]) + std::abs(pixel[1]) + std::abs(pixel[2])) / 3
					);
					alphaBound = std::max(alphaBound, std::abs(pixel[3]));
				}
			maximumMask = rgbBound * alphaBound;
		}
		const double radius = std::ceil((mask ? std::abs(size) : std::max(0., size)) * maximumMask);
		if (!std::isfinite(radius) || radius > 4000)
			return context.Fail(Status::LimitExceeded, "Non-Uniform Blur radius exceeds work budget", "size");
		const bool gradientEnabled = context.Boolean("use_gradient");
		const Value *gradientValue = gradientEnabled ? context.Find("gradient") : nullptr;
		const Gradient *gradient = gradientValue ? std::get_if<Gradient>(gradientValue) : nullptr;
		if (gradientEnabled && (!gradient || gradient->Keys.empty() || gradient->Keys.size() > 64))
			return context.Fail(
				Status::UnsupportedExecution, "Non-Uniform Blur gradient exceeds its GLSL slots", "gradient"
			);
		const GradientSampler gradientSampler =
			gradient ? ReadGradient(context, "gradient", *gradient) : GradientSampler{};
		SamplerSettings sampler = ReadSampler(context);
		const bool filtered = Filtered(sampler);
		sampler.Interpolation = filtered ? 2 : 1;
		const uint64_t span = uint64_t(radius) * 2 + 1;
		const uint64_t cost = span * span * (filtered ? 4 : 1) + 4 + (gradient ? gradient->Keys.size() : 0);
		const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		const uint64_t pixels = uint64_t(source->Width) * source->Height;
		if (cost > 64000000 || rows > 64000000 / cost || maskPixels > 64000000 / rows ||
			pixels > (64000000 / rows - maskPixels) / cost)
			return context.Fail(Status::LimitExceeded, "Non-Uniform Blur batch exceeds work budget", "size");
		const bool gamma = context.Boolean("gamma_correction"),
				   overrideColour = context.Boolean("override_color");
		const Colour overrideValue = context.Get<Colour>("color", {0, 0, 0, 255});
		const Rgba overridePixel{
			overrideValue.Red / 255.,
			overrideValue.Green / 255.,
			overrideValue.Blue / 255.,
			overrideValue.Alpha / 255.
		};
		return RunPixelProcessor(context, [&](const Image &surface, uint32_t, uint32_t, double u, double v) {
			if (context.FailureCode != Status::Ok) return Rgba{};
			double realSize = size;
			if (mask) {
				const Rgba point = Texture(*mask, u, v, false);
				realSize *= (point[0] + point[1] + point[2]) / 3 * point[3];
			}
			if (!std::isfinite(realSize)) {
				context.Fail(
					Status::UnsupportedExecution, "Non-Uniform Blur mapped radius is nonfinite", "blur_mask"
				);
				return Rgba{};
			}
			Rgba result = SampleTexture(surface, u, v, sampler);
			double totalWeight = 1, brightnessWeight = (result[0] + result[1] + result[2]) / 3 * result[3];
			if (realSize > 0) {
				const int extent = int(std::ceil(realSize));
				for (int i = -extent; i <= extent; ++i)
					for (int j = -extent; j <= extent; ++j) {
						if (i == 0 && j == 0) continue;
						if (std::abs(i + j) > extent * 2) continue;
						double weight = 1 - std::clamp((std::abs(i) + std::abs(j)) / (realSize * 2), 0., 1.);
						weight *= std::clamp(std::abs(i + j - std::floor(realSize) * 2), 0., 1.);
						Rgba sample = SampleTexture(
							surface, u + double(i) / surface.Width, v + double(j) / surface.Height, sampler
						);
						if (gamma)
							for (size_t channel = 0; channel < 3; ++channel) {
								if (sample[channel] < 0) {
									context.Fail(
										Status::UnsupportedExecution,
										"Non-Uniform Blur gamma power has a negative source base",
										"surface_in"
									);
									return Rgba{};
								}
								sample[channel] = std::pow(sample[channel], 2.2);
							}
						totalWeight += weight;
						brightnessWeight += weight * (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
						for (size_t channel = 0; channel < 4; ++channel)
							result[channel] += sample[channel] * weight;
					}
				for (double &channel : result)
					channel /= totalWeight;
			}
			// The center sample remains uncorrected before accumulation, even when gamma is enabled.
			if (gamma)
				for (size_t channel = 0; channel < 3; ++channel) {
					if (result[channel] < 0) {
						context.Fail(
							Status::UnsupportedExecution,
							"Non-Uniform Blur gamma power has a negative accumulation",
							"surface_in"
						);
						return Rgba{};
					}
					result[channel] = std::pow(result[channel], 1 / 2.2);
				}
			if (overrideColour) {
				for (size_t channel = 0; channel < 3; ++channel)
					result[channel] = overridePixel[channel];
				result[3] *= overridePixel[3];
			}
			if (gradientEnabled) {
				const double progress =
					ShaderFract(ShaderFract(1 - brightnessWeight / totalWeight + shift) + 1);
				const Rgba colour = GradientEval(gradientSampler, progress);
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] *= colour[channel];
			}
			for (double channel : result)
				if (!std::isfinite(channel)) {
					context.Fail(
						Status::UnsupportedExecution,
						"Non-Uniform Blur source result is nonfinite",
						"surface_in"
					);
					return Rgba{};
				}
			return result;
		});
	}
}
