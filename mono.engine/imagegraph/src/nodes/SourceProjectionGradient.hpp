#pragma once
#include "Gradient.hpp"
#include "SourceProjectionMath.hpp"
namespace engine::imagegraph::detail {
	using ProjectionColour = std::array<float, 4>;
	inline float ProjectionMix(float a, float b, float t) {
		return a * (1 - t) + b * t;
	}
	inline float ProjectionFract(float x) {
		return x - std::floor(x);
	}
	inline ProjectionVector ProjectionHsv(const ProjectionVector &c) {
		const std::array<float, 4> p = c[1] >= c[2] ? std::array<float, 4>{c[1], c[2], 0, -1 / 3.f}
													: std::array<float, 4>{c[2], c[1], -1, 2 / 3.f};
		const std::array<float, 4> q = c[0] >= p[0] ? std::array<float, 4>{c[0], p[1], p[2], p[0]}
													: std::array<float, 4>{p[0], p[1], p[3], c[0]};
		const float d = q[0] - std::min(q[3], q[1]), e = 1e-10f;
		return {std::abs(q[2] + (q[3] - q[1]) / (6 * d + e)), d / (q[0] + e), q[0]};
	}
	inline ProjectionVector ProjectionRgb(const ProjectionVector &c) {
		ProjectionVector rgb{};
		constexpr ProjectionVector k{1, 2 / 3.f, 1 / 3.f};
		for (size_t i = 0; i < 3; i++) {
			const float p = std::abs(ProjectionFract(c[0] + k[i]) * 6 - 3);
			rgb[i] = c[2] * ProjectionMix(1, std::clamp(p - 1, 0.f, 1.f), c[1]);
		}
		return rgb;
	}
	inline ProjectionVector
	ProjectionGradientMix(ProjectionVector a, ProjectionVector b, float t, int64_t mode) {
		ProjectionVector result{};
		if (mode == 2 || mode == 5) {
			a = ProjectionHsv(a);
			b = ProjectionHsv(b);
			const float delta = ProjectionFract(b[0] - a[0]);
			float shortest = ProjectionFract(2 * delta) - delta;
			if (mode == 5) shortest -= (shortest > 0) - (shortest < 0);
			return ProjectionRgb(
				{a[0] + shortest * t, ProjectionMix(a[1], b[1], t), ProjectionMix(a[2], b[2], t)}
			);
		}
		if (mode == 3) {
			constexpr ProjectionMatrix into{
				{{.4121656120f, .5362752080f, .0514575653f},
				 {.2118591070f, .6807189584f, .1074065790f},
				 {.0883097947f, .2818474174f, .6302613616f}}
			};
			constexpr ProjectionMatrix out{
				{{4.0767245293f, -3.3072168827f, .2307590544f},
				 {-1.2681437731f, 2.6093323231f, -.3411344290f},
				 {-.0041119885f, -.7034763098f, 1.7068625689f}}
			};
			for (size_t i = 0; i < 3; i++) {
				a[i] = std::pow(a[i], 2.2f);
				b[i] = std::pow(b[i], 2.2f);
			}
			a = ProjectionApply(into, a);
			b = ProjectionApply(into, b);
			for (size_t i = 0; i < 3; i++) {
				result[i] = ProjectionMix(
					std::pow(std::max(0.f, a[i]), 1 / 3.f), std::pow(std::max(0.f, b[i]), 1 / 3.f), t
				);
				result[i] = result[i] * result[i] * result[i];
			}
			result = ProjectionApply(out, result);
			for (float &v : result)
				v = std::pow(std::max(0.f, v), 1 / 2.2f);
			return result;
		}
		if (mode == 6) {
			const auto cmyk = [](const ProjectionVector &c) {
				const float k = 1 - std::max({c[0], c[1], c[2]});
				return ProjectionColour{
					(1 - c[0] - k) / (1 - k), (1 - c[1] - k) / (1 - k), (1 - c[2] - k) / (1 - k), k
				};
			};
			const auto ca = cmyk(a), cb = cmyk(b);
			const float k = ProjectionMix(ca[3], cb[3], t);
			for (size_t i = 0; i < 3; i++)
				result[i] = (1 - ProjectionMix(ca[i], cb[i], t)) * (1 - k);
			return result;
		}
		for (size_t i = 0; i < 3; i++) {
			result[i] = mode == 4
							? std::pow(ProjectionMix(std::pow(a[i], 2.2f), std::pow(b[i], 2.2f), t), 1 / 2.2f)
							: ProjectionMix(a[i], b[i], t);
		}
		return result;
	}
	inline ProjectionColour ProjectionGradient(const Gradient &gradient, float progress) {
		const auto colour = [&](size_t index) {
			const auto &c = gradient.Keys[index].Color;
			return ProjectionColour{c.Red / 255.f, c.Green / 255.f, c.Blue / 255.f, c.Alpha / 255.f};
		};
		for (size_t i = 0; i < gradient.Keys.size(); i++) {
			const float time = float(gradient.Keys[i].Time);
			if (time == progress) return colour(i);
			if (time > progress) {
				if (i == 0) return colour(0);
				const float t =
					(progress - float(gradient.Keys[i - 1].Time)) / (time - float(gradient.Keys[i - 1].Time));
				const auto a = colour(i - 1), b = colour(i);
				if (gradient.Mode == 1) return a;
				const auto c =
					ProjectionGradientMix({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, t, gradient.Mode);
				return {c[0], c[1], c[2], ProjectionMix(a[3], b[3], t)};
			}
		}
		return colour(gradient.Keys.size() - 1);
	}
}
