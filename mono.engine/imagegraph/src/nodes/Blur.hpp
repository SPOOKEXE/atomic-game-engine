#pragma once

// blurSurface.gml __gaussian_get_kernel and the two sh_blur_gaussian passes,
// shared by Blur, Shadow, Bloom and mask feathering paths that call
// surface_apply_gaussian.

#include "../PixelOpsGradient.hpp"
#include "../SurfaceScratch.hpp"
#include "Curve.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace engine::imagegraph::detail {
	// Native non-HLSL weight limit. Out-of-range taps use zero; device out-of-range
	// reads are unverified.
	inline constexpr size_t GAUSSIAN_WEIGHT_SLOTS = 256;

	inline std::vector<double> GaussianKernel(double size) {
		const int64_t count = static_cast<int64_t>(std::max(1.0, std::nearbyint(size)));
		const double spread = 0.3 * ((count - 1) * 0.5 - 1.0) + 0.8;
		std::vector<double> weights(static_cast<size_t>(count));
		double total = 0.0;
		for (int64_t index = 0; index < count; index++) {
			const double x = index * 0.5;
			weights[static_cast<size_t>(index)] = (1.0 / std::sqrt(2.0 * std::numbers::pi * spread)) *
												  std::exp(-(x * x) / (2.0 * spread * spread));
			total += index ? weights[static_cast<size_t>(index)] * 2.0 : weights[static_cast<size_t>(index)];
		}
		for (double &weight : weights)
			weight /= total;
		return weights;
	}

	struct GaussianArgs {
		// Size, or the low end of the mapped range.
		double Size = 0.0;
		// High end of the mapped range. Equal to Size when unmapped.
		double SizeHigh = 0.0;
		double KernelLimit = 1024.0;
		const Image *SizeMap = nullptr;
		// sampleMode uniform: 1 Empty outside [0, 1] unless a node passes its
		// Oversample attribute.
		int64_t SampleMode = 1;
		bool Gamma = false;
		std::optional<Rgba> OverrideColour;
		// sizeModulate of the vertical pass.
		double Ratio = 1.0;
		double AngleRadians = 0.0;
		const Image *UvMap = nullptr;
		double UvMix = 0.0;
		// A replacement kernel, such as Blur's normalized intensity curve.
		std::optional<std::vector<double>> Kernel;
	};

	// sampleTexture with the UV map blend used by sh_blur_gaussian (mapBlend is
	// index / size).
	inline Rgba
	BlurSample(const Image &image, double u, double v, double mapBlend, const GaussianArgs &args) {
		const auto invalid = [] {
			const double nan = std::numeric_limits<double>::quiet_NaN();
			return Rgba{nan, nan, nan, nan};
		};
		if (!std::isfinite(u) || !std::isfinite(v)) return invalid();
		if (args.UvMap) {
			const Rgba map = BilinearClamp(*args.UvMap, u, v);
			// A zero-size pass divides 0 by 0 here; the shader result is undefined, so
			// the map is skipped.
			const double amount = std::isfinite(mapBlend) ? mapBlend * args.UvMix : 0.0;
			u += (map[0] - u) * amount;
			v += ((1.0 - map[1]) - v) * amount;
		}
		if (!std::isfinite(u) || !std::isfinite(v)) return invalid();
		return SampleTextureSimple(image, u, v, args.SampleMode, true);
	}

	inline bool GaussianPass(
		const Image &source,
		Image &target,
		bool horizontal,
		double modulate,
		const GaussianArgs &args,
		const std::vector<double> &kernel
	) {
		const double strMax = std::max(args.Size, args.SizeHigh);
		const double cosine = std::cos(args.AngleRadians), sine = std::sin(args.AngleRadians);
		const auto weightAt = [&](int64_t index) {
			if (index < 0 || static_cast<size_t>(index) >= std::min(kernel.size(), GAUSSIAN_WEIGHT_SLOTS))
				return 0.0;
			return kernel[static_cast<size_t>(index)];
		};
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				const double u = (x + 0.5) / source.Width, v = (y + 0.5) / source.Height;
				double strength = args.Size;
				if (args.SizeMap) {
					const Rgba map = BilinearClamp(*args.SizeMap, u, v);
					strength = args.Size + (args.SizeHigh - args.Size) * ((map[0] + map[1] + map[2]) / 3.0);
				}
				strength *= modulate;
				double weightedAlpha = 0.00001, totalWeight = 0.00001;
				Rgba result{};
				const auto sample = [&](double su, double sv, double index) {
					if (!std::isfinite(index)) {
						result.fill(std::numeric_limits<double>::quiet_NaN());
						return;
					}
					const double fraction = index - std::floor(index);
					const auto whole = static_cast<int64_t>(std::floor(index));
					const double weight =
						weightAt(whole) + (weightAt(whole + 1) - weightAt(whole)) * fraction;
					Rgba colour = BlurSample(source, su, sv, index / strength, args);
					if (args.Gamma)
						for (size_t channel = 0; channel < 3; channel++)
							colour[channel] = std::pow(std::abs(colour[channel]), 2.2);
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] *= weight * colour[3];
					weightedAlpha += weight * colour[3];
					totalWeight += weight;
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] += colour[channel];
				};
				sample(u, v, 0.0);
				for (double step = 1.0; step < strMax; step++) {
					if (step > strength) break;
					// rot * vec2(offset): rot = mat2(cos, -sin, sin, cos) in column-major
					// GLSL.
					const double ox = horizontal ? step / source.Width : 0.0;
					const double oy = horizontal ? 0.0 : step / source.Height;
					const double dx = cosine * ox + sine * oy, dy = -sine * ox + cosine * oy;
					const double index = step / strength * strMax;
					sample(u + dx, v + dy, index);
					sample(u - dx, v - dy, index);
				}
				for (size_t channel = 0; channel < 3; channel++) {
					result[channel] /= weightedAlpha;
					if (args.Gamma) result[channel] = std::pow(result[channel], 1.0 / 2.2);
				}
				result[3] = weightedAlpha / totalWeight;
				if (args.OverrideColour) {
					for (size_t channel = 0; channel < 3; channel++)
						result[channel] = (*args.OverrideColour)[channel];
					result[3] *= (*args.OverrideColour)[3];
				}
				if (!WritePixel(target, x, y, result)) return false;
			}
		}
		return true;
	}

	// The source allocates both passes in the input format before any final output
	// conversion.
	inline std::optional<SurfaceScratch>
	GaussianBlur(NodeContext &context, const Image &source, const GaussianArgs &args) {
		if (!ValidSurfaceLayout(source, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
			!std::isfinite(args.Size) || !std::isfinite(args.SizeHigh) ||
			static_cast<long double>(source.Width) * source.Height *
					std::max(1.0, std::max(args.Size, args.SizeHigh)) * 4 >
				256'000'000) {
			context.Fail(Status::LimitExceeded, "Gaussian source or size exceeds work budget", "size");
			return std::nullopt;
		}
		auto kernelCharge = context.ReserveWorkspace(0, "size");
		if (!kernelCharge) return std::nullopt;
		std::vector<double> generated;
		if (!args.Kernel) {
			const double size = std::min(args.KernelLimit, std::max(args.Size, args.SizeHigh));
			if (!std::isfinite(size)) {
				context.Fail(Status::InvalidValue, "Gaussian size must be finite", "size");
				return std::nullopt;
			}
			const uint64_t count = uint64_t(std::max(1.0, std::nearbyint(size)));
			auto required = context.ReserveWorkspace(count * sizeof(double), "size");
			if (!required) return std::nullopt;
			*kernelCharge = std::move(*required);
			generated = GaussianKernel(size);
		}
		const auto &kernel = args.Kernel ? *args.Kernel : generated;
		auto horizontal =
			MakeSurfaceScratch(context, source.Width, source.Height, source.Format, "blur_horizontal");
		if (!horizontal) return std::nullopt;
		auto vertical =
			MakeSurfaceScratch(context, source.Width, source.Height, source.Format, "blur_vertical");
		if (!vertical) return std::nullopt;
		if (!GaussianPass(source, horizontal->Data, true, 1.0, args, kernel) ||
			!GaussianPass(horizontal->Data, vertical->Data, false, args.Ratio, args, kernel)) {
			context.Fail(
				Status::InvalidValue, "Gaussian sample exceeds numeric surface range", "surface_out"
			);
			return std::nullopt;
		}
		return vertical;
	}

	// Preserve the existing CPU gradient interpolation with bounded,
	// allocation-free scratch.
	inline Rgba BlurGradientTint(const Gradient &gradient, double progress) {
		std::array<engine::imagegraph::detail::GradientKey, Limits::MaximumGradientKeys> keys{};
		const size_t count = std::min(gradient.Keys.size(), keys.size());
		for (size_t index = 0; index < count; ++index)
			keys[index] = {gradient.Keys[index].Time, gradient.Keys[index].Color};
		std::array<double, 4> rgba{};
		if (GradientColor(
				std::span<const engine::imagegraph::detail::GradientKey>(keys.data(), count),
				gradient.Mode,
				progress,
				rgba
			) != GradientStatus::Ok)
			return {};
		return {rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0, rgba[3] / 255.0};
	}

	struct DirectionalArgs {
		// Strength in pixels over 128, or the low end of its mapped range.
		double Strength = 0.0, StrengthHigh = 0.0;
		const Image *StrengthMap = nullptr;
		// Direction in degrees, or the low end of its mapped range.
		double Direction = 0.0, DirectionHigh = 0.0;
		const Image *DirectionMap = nullptr;
		double Resolution = 1.0;
		bool Single = false, Fade = false, Gamma = false;
		int64_t SampleMode = 2;
		const Image *UvMap = nullptr;
		double UvMix = 0.0;
		const Curve *StrengthCurve = nullptr;
		int64_t Spectral = 0;
		double SpectralIntensity = 0.0, SpectralShift = 0.0, SpectralScale = 1.0;
		// Colour for Spectral mode 2, evaluated by the caller's gradient.
		const Gradient *SpectralGradient = nullptr;
	};

	inline std::array<double, 3> SpectralZucconi6(double x) {
		constexpr std::array<double, 3> C1{3.54585104, 2.93225262, 2.41593945},
			X1{0.69549072, 0.49228336, 0.27699880}, Y1{0.02312639, 0.15225084, 0.52607955},
			C2{3.90307140, 3.21182957, 3.96587128}, X2{0.11748627, 0.86755042, 0.66077860},
			Y2{0.84897130, 0.88445281, 0.73949448};
		std::array<double, 3> result{};
		for (size_t channel = 0; channel < 3; channel++) {
			const auto bump = [](double value, double offset) {
				return std::clamp(1.0 - value * value - offset, 0.0, 1.0);
			};
			result[channel] = bump(C1[channel] * (x - X1[channel]), Y1[channel]) +
							  bump(C2[channel] * (x - X2[channel]), Y2[channel]);
		}
		return result;
	}

	// sh_blur_directional under gpu_set_tex_filter(true). The tap loop keeps the
	// shader's float steps, so the tap count matches single-precision accumulation.
	inline std::optional<SurfaceScratch>
	DirectionalBlur(NodeContext &context, const Image &source, const DirectionalArgs &args) {
		if (!(args.Resolution > 0) || !std::isfinite(args.Resolution) ||
			static_cast<long double>(source.Width) * source.Height * std::max(source.Width, source.Height) *
					2 * args.Resolution >
				256'000'000) {
			context.Fail(Status::LimitExceeded, "directional blur exceeds work budget", "resolution");
			return std::nullopt;
		}
		const float size = static_cast<float>(std::max(source.Width, source.Height));
		const float delta = 1.0f / size / static_cast<float>(args.Resolution);
		if (!(delta > 0) || !std::isfinite(delta) || 1.0f + delta == 1.0f) {
			context.Fail(Status::LimitExceeded, "directional float tap step cannot advance", "resolution");
			return std::nullopt;
		}
		auto scratch = MakeSurfaceScratch(context, source.Width, source.Height, source.Format, "surface_out");
		if (!scratch) return std::nullopt;
		Image &output = scratch->Data;
		GaussianArgs sampling;
		sampling.UvMap = args.UvMap;
		sampling.UvMix = args.UvMix;
		sampling.SampleMode = args.SampleMode;
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				const double u = (x + 0.5) / source.Width, v = (y + 0.5) / source.Height;
				double strength = args.Strength;
				if (args.StrengthMap) {
					const Rgba map = BilinearClamp(*args.StrengthMap, u, v);
					strength = args.Strength +
							   (args.StrengthHigh - args.Strength) * ((map[0] + map[1] + map[2]) / 3.0);
				}
				strength /= 128.0;
				double direction = args.Direction;
				if (args.DirectionMap) {
					const Rgba map = BilinearClamp(*args.DirectionMap, u, v);
					direction = args.Direction +
								(args.DirectionHigh - args.Direction) * ((map[0] + map[1] + map[2]) / 3.0);
				}
				const double radians = (direction + 90.0) * std::numbers::pi / 180.0;
				const double angleX = std::sin(radians) * strength, angleY = std::cos(radians) * strength;
				Rgba result{};
				std::array<double, 3> spectrum{};
				double weight = 0.0, iterations = 0.0;
				for (float step = args.Single ? 0.0f : -1.0f; step <= 1.0f; step += delta) {
					Rgba colour =
						BlurSample(source, u - angleX * step, v - angleY * step, std::abs(step), sampling);
					if (args.Gamma)
						for (size_t channel = 0; channel < 3; channel++)
							colour[channel] = std::pow(colour[channel], 2.2);
					const double fade = args.Fade ? 1.0 - std::abs(step) : 1.0;
					const double amplitude =
						args.StrengthCurve ? EvalShaderCurve(*args.StrengthCurve, std::abs(step)) : 1.0;
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] *= fade;
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] += colour[channel] * amplitude;
					weight += colour[3] * fade;
					iterations += fade;
					if (args.Spectral == 0) continue;
					const double scaled = std::abs(step * args.SpectralScale) + args.SpectralShift;
					const double offset = scaled - std::floor(scaled);
					const double intensity =
						fade * amplitude *
						std::sqrt(colour[0] * colour[0] + colour[1] * colour[1] + colour[2] * colour[2]) *
						colour[3] * args.SpectralIntensity;
					if (args.Spectral == 1) {
						const auto tint = SpectralZucconi6(offset);
						for (size_t channel = 0; channel < 3; channel++)
							spectrum[channel] += tint[channel] * intensity;
					} else if (args.Spectral == 2 && args.SpectralGradient) {
						const Rgba tint = BlurGradientTint(*args.SpectralGradient, offset);
						for (size_t channel = 0; channel < 3; channel++)
							spectrum[channel] += tint[channel] * intensity;
					}
				}
				for (size_t channel = 0; channel < 3; channel++) {
					result[channel] = (result[channel] + spectrum[channel]) / weight;
					if (args.Gamma) result[channel] = std::pow(result[channel], 1.0 / 2.2);
				}
				result[3] /= iterations;
				if (!WritePixel(output, x, y, result)) {
					context.Fail(
						Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
					);
					return std::nullopt;
				}
			}
		}
		return scratch;
	}

	struct ZoomArgs {
		double Strength = 0.0, StrengthHigh = 0.0;
		const Image *StrengthMap = nullptr;
		// Zoom origin in pixels.
		double CenterX = 0.0, CenterY = 0.0;
		// 0 Start, 1 Middle, 2 End.
		int64_t BlurMode = 0;
		int64_t SampleMode = 0;
		int64_t Samples = 64;
		bool Gamma = false, Fade = true, Step = false;
		const Image *Mask = nullptr;
		const Image *UvMap = nullptr;
		double UvMix = 0.0;
		const Curve *StrengthCurve = nullptr;
		int64_t Spectral = 0;
		double SpectralIntensity = 0.0, SpectralShift = 0.0, SpectralScale = 1.0;
		const Gradient *SpectralGradient = nullptr;
	};

	// sh_blur_zoom, or sh_blur_zoom_step which keeps the first non-transparent tap.
	// Unfiltered reads.
	inline std::optional<SurfaceScratch>
	ZoomBlur(NodeContext &context, const Image &source, const ZoomArgs &args) {
		if (args.Samples < 1 ||
			static_cast<long double>(source.Width) * source.Height * args.Samples * 2 > 256'000'000) {
			context.Fail(Status::LimitExceeded, "zoom blur exceeds work budget", "samples");
			return std::nullopt;
		}
		const float samples = static_cast<float>(args.Samples);
		const float amount = samples * 2.0f + 1.0f;
		// A float counter stops advancing at 2^24; retain the source loop only below
		// that boundary.
		if (!std::isfinite(amount) || amount > 16'777'216.0f) {
			context.Fail(Status::LimitExceeded, "zoom blur sample counter cannot advance", "samples");
			return std::nullopt;
		}
		auto scratch = MakeSurfaceScratch(context, source.Width, source.Height, source.Format, "surface_out");
		if (!scratch) return std::nullopt;
		Image &output = scratch->Data;
		GaussianArgs sampling;
		sampling.UvMap = args.UvMap;
		sampling.UvMix = args.UvMix;
		sampling.SampleMode = args.SampleMode;
		const double half = std::max(source.Width, source.Height) / 2.0;
		const double centerU = args.CenterX / source.Width, centerV = args.CenterY / source.Height;
		const float start = args.BlurMode == 1	 ? -samples
							: args.BlurMode == 2 ? -samples * 2.0f - 1.0f
												 : 0.0f;
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				const double u = (x + 0.5) / source.Width, v = (y + 0.5) / source.Height;
				double strength = args.Strength;
				if (args.StrengthMap) {
					const Rgba map = BilinearClamp(*args.StrengthMap, u, v);
					strength = args.Strength +
							   (args.StrengthHigh - args.Strength) * ((map[0] + map[1] + map[2]) / 3.0);
				}
				strength /= half;
				if (args.Mask) {
					const Rgba mask = SampleNearest(*args.Mask, u, v);
					strength *= (mask[0] + mask[1] + mask[2]) / 3.0 * mask[3];
				}
				const double factor = strength * (1.0 / (samples * 2.0 - 1.0));
				const double du = u - centerU, dv = v - centerV;
				Rgba result{}, first{};
				std::array<double, 3> spectrum{};
				double weight = 0.0, iterations = 0.0;
				for (float step = 0.0f; step < amount; step++) {
					const double scale = 1.0 + (start + step) * factor;
					const double ratio = step / amount;
					Rgba colour =
						BlurSample(source, du * scale + centerU, dv * scale + centerV, ratio, sampling);
					if (args.Step) {
						const double scaled = ratio * args.SpectralScale + args.SpectralShift;
						const double offset = scaled - std::floor(scaled);
						if (args.Spectral == 1) {
							const auto tint = SpectralZucconi6(offset);
							for (size_t channel = 0; channel < 3; channel++)
								colour[channel] *= tint[channel];
						} else if (args.Spectral == 2 && args.SpectralGradient) {
							const Rgba tint = BlurGradientTint(*args.SpectralGradient, offset);
							for (size_t channel = 0; channel < 4; channel++)
								colour[channel] *= tint[channel];
						}
						if (colour[3] > 0.0) {
							first = colour;
							break;
						}
						continue;
					}
					if (args.Gamma)
						for (size_t channel = 0; channel < 3; channel++)
							colour[channel] = std::pow(colour[channel], 2.2);
					const double fade = args.Fade ? 1.0 - ratio : 1.0;
					const double amplitude =
						args.StrengthCurve ? EvalShaderCurve(*args.StrengthCurve, ratio) : 1.0;
					for (size_t channel = 0; channel < 3; channel++)
						colour[channel] *= fade;
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] += colour[channel] * amplitude;
					weight += colour[3] * fade;
					iterations += fade;
					if (args.Spectral == 0) continue;
					const double scaled =
						std::abs((ratio - 0.5) * 2.0 * args.SpectralScale) + args.SpectralShift;
					const double offset = scaled - std::floor(scaled);
					const double intensity =
						fade * amplitude *
						std::sqrt(colour[0] * colour[0] + colour[1] * colour[1] + colour[2] * colour[2]) *
						colour[3] * args.SpectralIntensity;
					if (args.Spectral == 1) {
						const auto tint = SpectralZucconi6(offset);
						for (size_t channel = 0; channel < 3; channel++)
							spectrum[channel] += tint[channel] * intensity;
					} else if (args.Spectral == 2 && args.SpectralGradient) {
						const Rgba tint = BlurGradientTint(*args.SpectralGradient, offset);
						for (size_t channel = 0; channel < 3; channel++)
							spectrum[channel] += tint[channel] * intensity;
					}
				}
				if (args.Step) {
					if (!WritePixel(output, x, y, first)) {
						context.Fail(
							Status::InvalidValue, "zoom sample exceeds numeric surface range", "surface_out"
						);
						return std::nullopt;
					}
					continue;
				}
				for (size_t channel = 0; channel < 3; channel++) {
					result[channel] = (result[channel] + spectrum[channel]) / weight;
					if (args.Gamma) result[channel] = std::pow(result[channel], 1.0 / 2.2);
				}
				result[3] /= iterations;
				if (!WritePixel(output, x, y, result)) {
					context.Fail(
						Status::InvalidValue, "blur sample exceeds numeric surface range", "surface_out"
					);
					return std::nullopt;
				}
			}
		}
		return scratch;
	}
} // namespace engine::imagegraph::detail
