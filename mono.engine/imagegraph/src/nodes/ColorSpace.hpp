#pragma once

// The source shaders' shared colour space region (rgb2hsv, hsv2rgb), in double precision.

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	using Rgb3 = std::array<double, 3>;

	inline Rgb3 ShaderRgbToHsv(const Rgb3 &c) {
		const std::array<double, 4> k{0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0};
		const double stepGB = c[2] <= c[1] ? 1.0 : 0.0;
		const std::array<double, 4> p = stepGB ? std::array<double, 4>{c[1], c[2], k[0], k[1]}
											   : std::array<double, 4>{c[2], c[1], k[3], k[2]};
		const double stepPR = p[0] <= c[0] ? 1.0 : 0.0;
		const std::array<double, 4> q = stepPR ? std::array<double, 4>{c[0], p[1], p[2], p[0]}
											   : std::array<double, 4>{p[0], p[1], p[3], c[0]};
		const double d = q[0] - std::min(q[3], q[1]);
		const double e = 0.0000000001;
		return {std::abs(q[2] + (q[3] - q[1]) / (6.0 * d + e)), d / (q[0] + e), q[0]};
	}

	inline Rgb3 ShaderHsvToRgb(const Rgb3 &c) {
		const std::array<double, 4> k{1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0};
		Rgb3 result{};
		for (size_t channel = 0; channel < 3; channel++) {
			const double shifted = c[0] + k[channel];
			const double p = std::abs((shifted - std::floor(shifted)) * 6.0 - k[3]);
			result[channel] = c[2] * (k[0] + (std::clamp(p - k[0], 0.0, 1.0) - k[0]) * c[1]);
		}
		return result;
	}
}
