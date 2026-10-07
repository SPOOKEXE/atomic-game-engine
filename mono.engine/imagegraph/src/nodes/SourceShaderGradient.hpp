#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace engine::imagegraph::detail {
	using ShaderGradientRgb3 = std::array<float, 3>;
	using ShaderGradientRgba = std::array<float, 4>;
	inline float ShaderGradientMixLane(float a, float b, float t) {
		return a * (1.f - t) + b * t;
	}
	inline float ShaderGradientFract(float x) {
		return x - std::floor(x);
	}
	// grug shader gradient color-space math stays float, without host color-byte rounding.
	inline ShaderGradientRgb3 ShaderGradientHsv(ShaderGradientRgb3 c) {
		ShaderGradientRgba p = c[2] <= c[1] ? ShaderGradientRgba{c[1], c[2], 0, -1.f / 3.f}
											: ShaderGradientRgba{c[2], c[1], -1, 2.f / 3.f};
		ShaderGradientRgba q = p[0] <= c[0] ? ShaderGradientRgba{c[0], p[1], p[2], p[0]}
											: ShaderGradientRgba{p[0], p[1], p[3], c[0]};
		const float d = q[0] - std::min(q[3], q[1]), e = .0000000001f;
		return {std::abs(q[2] + (q[3] - q[1]) / (6.f * d + e)), d / (q[0] + e), q[0]};
	}
	inline ShaderGradientRgb3 ShaderGradientRgb(ShaderGradientRgb3 c) {
		ShaderGradientRgb3 out{};
		constexpr ShaderGradientRgb3 offset{1, 2.f / 3.f, 1.f / 3.f};
		for (size_t lane = 0; lane < 3; ++lane) {
			const float p = std::abs(ShaderGradientFract(c[0] + offset[lane]) * 6.f - 3.f);
			out[lane] = c[2] * ShaderGradientMixLane(1.f, std::clamp(p - 1.f, 0.f, 1.f), c[1]);
		}
		return out;
	}
	inline ShaderGradientRgb3
	ShaderGradientMix(ShaderGradientRgb3 a, ShaderGradientRgb3 b, float t, int64_t mode) {
		ShaderGradientRgb3 out{};
		if (mode == 2 || mode == 5) {
			a = ShaderGradientHsv(a);
			b = ShaderGradientHsv(b);
			const float delta = ShaderGradientFract(b[0] - a[0]);
			float shortest = ShaderGradientFract(2.f * delta) - delta;
			if (mode == 5) shortest -= float(shortest > 0) - float(shortest < 0);
			return ShaderGradientRgb(
				{a[0] + shortest * t,
				 ShaderGradientMixLane(a[1], b[1], t),
				 ShaderGradientMixLane(a[2], b[2], t)}
			);
		}
		if (mode == 3) {
			const auto toLms = [](ShaderGradientRgb3 c) {
				for (auto &v : c)
					v = std::pow(v, 2.2f);
				return ShaderGradientRgb3{
					std::pow(
						std::max(0.f, .4121656120f * c[0] + .5362752080f * c[1] + .0514575653f * c[2]),
						1.f / 3.f
					),
					std::pow(
						std::max(0.f, .2118591070f * c[0] + .6807189584f * c[1] + .1074065790f * c[2]),
						1.f / 3.f
					),
					std::pow(
						std::max(0.f, .0883097947f * c[0] + .2818474174f * c[1] + .6302613616f * c[2]),
						1.f / 3.f
					)
				};
			};
			a = toLms(a);
			b = toLms(b);
			for (size_t i = 0; i < 3; ++i) {
				out[i] = ShaderGradientMixLane(a[i], b[i], t);
				out[i] = out[i] * out[i] * out[i];
			}
			return {
				std::pow(
					std::max(0.f, 4.0767245293f * out[0] - 3.3072168827f * out[1] + .2307590544f * out[2]),
					1.f / 2.2f
				),
				std::pow(
					std::max(0.f, -1.2681437731f * out[0] + 2.6093323231f * out[1] - .3411344290f * out[2]),
					1.f / 2.2f
				),
				std::pow(
					std::max(0.f, -.0041119885f * out[0] - .7034763098f * out[1] + 1.7068625689f * out[2]),
					1.f / 2.2f
				)
			};
		}
		if (mode == 6) {
			const auto toCmyk = [](ShaderGradientRgb3 c) {
				const float k = 1.f - std::max({c[0], c[1], c[2]});
				return ShaderGradientRgba{
					(1.f - c[0] - k) / (1.f - k),
					(1.f - c[1] - k) / (1.f - k),
					(1.f - c[2] - k) / (1.f - k),
					k
				};
			};
			const auto x = toCmyk(a), y = toCmyk(b);
			ShaderGradientRgba m{};
			for (size_t i = 0; i < 4; ++i)
				m[i] = ShaderGradientMixLane(x[i], y[i], t);
			return {(1.f - m[0]) * (1.f - m[3]), (1.f - m[1]) * (1.f - m[3]), (1.f - m[2]) * (1.f - m[3])};
		}
		for (size_t i = 0; i < 3; ++i)
			out[i] = mode == 4 ? std::pow(
									 ShaderGradientMixLane(std::pow(a[i], 2.2f), std::pow(b[i], 2.2f), t),
									 1.f / 2.2f
								 )
							   : ShaderGradientMixLane(a[i], b[i], t);
		return out;
	}
}
