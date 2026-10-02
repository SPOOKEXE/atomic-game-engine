#pragma once

// The shader gradient region (gradientEval) uploaded by gradientObject.shader_submit and shader_set_gradient.
// GLSL holds 64 keys; unset slots read time 0. A mapped gradient samples its map, filtered, along the range.

#include "ColorSpace.hpp"
#include "Processor.hpp"

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	inline constexpr size_t GRADIENT_KEY_SLOTS = 64;

	inline Rgb3 OklabFromRgb(const Rgb3 &colour) {
		// mat3(...) is column-major: each group of three literals is one column.
		constexpr double M[3][3] = {
			{0.4121656120, 0.2118591070, 0.0883097947},
			{0.5362752080, 0.6807189584, 0.2818474174},
			{0.0514575653, 0.1074065790, 0.6302613616},
		};
		const Rgb3 linear{std::pow(colour[0], 2.2), std::pow(colour[1], 2.2), std::pow(colour[2], 2.2)};
		Rgb3 lms{};
		for (size_t row = 0; row < 3; row++)
			lms[row] = std::pow(std::max(0.0, M[0][row] * linear[0] + M[1][row] * linear[1] + M[2][row] * linear[2]), 1.0 / 3.0);
		return lms;
	}

	inline Rgb3 RgbFromOklab(const Rgb3 &lms) {
		constexpr double M[3][3] = {
			{4.0767245293, -1.2681437731, -0.0041119885},
			{-3.3072168827, 2.6093323231, -0.7034763098},
			{0.2307590544, -0.3411344290, 1.7068625689},
		};
		const Rgb3 cube{lms[0] * lms[0] * lms[0], lms[1] * lms[1] * lms[1], lms[2] * lms[2] * lms[2]};
		Rgb3 colour{};
		for (size_t row = 0; row < 3; row++)
			colour[row] = std::pow(std::max(0.0, M[0][row] * cube[0] + M[1][row] * cube[1] + M[2][row] * cube[2]), 1.0 / 2.2);
		return colour;
	}

	inline double ShaderFract(double value) {
		return value - std::floor(value);
	}

	inline double HueLerp(double a0, double a1, double t, bool inverse) {
		const double delta = ShaderFract(a1 - a0);
		double shortest = ShaderFract(2.0 * delta) - delta;
		if (inverse) shortest -= (shortest > 0) - (shortest < 0);
		return a0 + shortest * t;
	}

	inline Rgb3 GradientMix(const Rgb3 &c0, const Rgb3 &c1, double t, int64_t blend) {
		Rgb3 result{};
		switch (blend) {
		case 2:
		case 5: {
			const Rgb3 h0 = ShaderRgbToHsv(c0), h1 = ShaderRgbToHsv(c1);
			return ShaderHsvToRgb(
				{HueLerp(h0[0], h1[0], t, blend == 5), h0[1] + (h1[1] - h0[1]) * t, h0[2] + (h1[2] - h0[2]) * t}
			);
		}
		case 3: {
			const Rgb3 k0 = OklabFromRgb(c0), k1 = OklabFromRgb(c1);
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] = k0[channel] + (k1[channel] - k0[channel]) * t;
			return RgbFromOklab(result);
		}
		case 4:
			for (size_t channel = 0; channel < 3; channel++) {
				const double k0 = std::pow(c0[channel], 2.2), k1 = std::pow(c1[channel], 2.2);
				result[channel] = std::pow(k0 + (k1 - k0) * t, 1.0 / 2.2);
			}
			return result;
		case 6: {
			const auto cmyk = [](const Rgb3 &c) {
				const double k = 1.0 - std::max({c[0], c[1], c[2]});
				return std::array<double, 4>{(1.0 - c[0] - k) / (1.0 - k), (1.0 - c[1] - k) / (1.0 - k),
											 (1.0 - c[2] - k) / (1.0 - k), k};
			};
			const auto a = cmyk(c0), b = cmyk(c1);
			std::array<double, 4> mixed{};
			for (size_t channel = 0; channel < 4; channel++)
				mixed[channel] = a[channel] + (b[channel] - a[channel]) * t;
			return {(1.0 - mixed[0]) * (1.0 - mixed[3]), (1.0 - mixed[1]) * (1.0 - mixed[3]), (1.0 - mixed[2]) * (1.0 - mixed[3])};
		}
		default:
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] = c0[channel] + (c1[channel] - c0[channel]) * t;
			return result;
		}
	}

	struct GradientSampler {
		const Gradient *Keys = nullptr;
		// With a map, the gradient samples it along Range (x0, y0) to (x1, y1).
		const Image *Map = nullptr;
		Vector4 Range{0, 0, 1, 1};
	};

	inline Rgba GradientEval(const GradientSampler &sampler, double progress) {
		if (sampler.Map) {
			const Vector4 &range = sampler.Range;
			return BilinearClamp(
				*sampler.Map, range.X + (range.Z - range.X) * progress, range.Y + (range.W - range.Y) * progress
			);
		}
		const Gradient &gradient = *sampler.Keys;
		const size_t keys = std::min(gradient.Keys.size(), GRADIENT_KEY_SLOTS);
		const auto colour = [&](size_t index) {
			if (index >= keys) return Rgba{};
			const Colour &value = gradient.Keys[index].Color;
			return Rgba{value.Red / 255.0, value.Green / 255.0, value.Blue / 255.0, value.Alpha / 255.0};
		};
		const auto time = [&](size_t index) { return index < keys ? gradient.Keys[index].Time : 0.0; };
		if (keys == 0) return Rgba{};
		for (size_t index = 0; index < GRADIENT_KEY_SLOTS; index++) {
			if (time(index) == progress) return colour(index);
			if (time(index) > progress) {
				if (index == 0) return colour(0);
				const double t = (progress - time(index - 1)) / (time(index) - time(index - 1));
				const Rgba before = colour(index - 1), after = colour(index);
				if (gradient.Mode == 1) return before;
				const Rgb3 mixed = GradientMix({before[0], before[1], before[2]}, {after[0], after[1], after[2]}, t, gradient.Mode);
				return Rgba{mixed[0], mixed[1], mixed[2], before[3] + (after[3] - before[3]) * t};
			}
			if (index + 1 >= keys) return colour(keys - 1);
		}
		return colour(keys - 1);
	}

	// shader_set_gradient: the map applies while "<id>_mapped" is on and "<id>_map" is linked.
	inline GradientSampler ReadGradient(const NodeContext &context, std::string_view id, const Gradient &keys) {
		GradientSampler sampler;
		sampler.Keys = &keys;
		const std::string base(id);
		const Image *map = context.Input(base + "_map");
		if (map && context.Boolean(base + "_mapped")) {
			sampler.Map = map;
			sampler.Range = context.Get<Vector4>(base + "_map_range", Vector4{0, 0, 1, 1});
		}
		return sampler;
	}
}
