#pragma once
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
namespace engine::imagegraph::detail {
	inline float NormalShaderCurveSegmentT(float y0, float ay0, float by1, float y1, float t) {
		const float rest = 1 - t;
		return y0 * rest * rest * rest + ay0 * 3 * rest * rest * t + by1 * 3 * rest * t * t + y1 * t * t * t;
	}
	inline float
	NormalShaderCurveSegmentX(float y0, float ax0, float ay0, float bx1, float by1, float y1, float x) {
		if (x <= 0.0f) return y0;
		if (x >= 1.0f) return y1;
		if (y0 == ay0 && y0 == by1 && y0 == y1) return y0;
		float t = x;
		for (int repeat = 0; repeat < 8; repeat++) {
			const float rest = 1.0f - t;
			const float ft = 3.0f * rest * rest * t * ax0 + 3.0f * rest * t * t * bx1 + t * t * t;
			if (std::abs(ft - x) < 0.0001f) return NormalShaderCurveSegmentT(y0, ay0, by1, y1, t);
			const float slope =
				3.0f * rest * rest * ax0 + 6.0f * rest * t * (bx1 - ax0) + 3.0f * t * t * (1.0f - bx1);
			t = t - (ft - x) / slope;
		}
		return NormalShaderCurveSegmentT(y0, ay0, by1, y1, t);
	}

	// curveEval from the shader curve region: the header's range wraps _curveEval, whose step mode uses a
	// strict comparison. GLSL holds 64 floats, so curves beyond nine anchors are undefined in the source.
	inline float NormalEvalShaderCurve(const Curve &curve, float x) {
		float minimum = float(curve.Header[3]), maximum = float(curve.Header[4]);
		if (minimum == 0.0f && maximum == 0.0f) maximum = 1.0f;
		const auto evaluate = [&]() -> float {
			if (curve.Anchors.empty()) return 0.0f;
			const size_t segments = curve.Anchors.size() - 1;
			const float type = float(curve.Header[2]);
			x = std::clamp(x / float(curve.Header[1]) - float(curve.Header[0]), 0.0f, 1.0f);
			if (x <= float(curve.Anchors.front()[2])) return float(curve.Anchors.front()[3]);
			if (x >= float(curve.Anchors.back()[2])) return float(curve.Anchors.back()[3]);
			if (type == 0.0f) {
				for (size_t segment = 0; segment < segments; segment++) {
					const auto &from = curve.Anchors[segment];
					const auto &to = curve.Anchors[segment + 1];
					const float x0 = float(from[2]), y0 = float(from[3]), x1 = float(to[2]),
								y1 = float(to[3]);
					if (x < x0 || x > x1) continue;
					float dx0 = float(from[4]), dy0 = float(from[5]), dx1 = float(to[0]), dy1 = float(to[1]);
					if (std::abs(dx0) + std::abs(dx1) > std::abs(x0 - x1) * 2.0f) {
						const float ratio = (std::abs(x0 - x1) * 2.0f) / (std::abs(dx0) + std::abs(dx1));
						dx0 *= ratio;
						dx1 *= ratio;
					}
					const float width = x1 - x0;
					const float t = (x - x0) / width;
					if (dx0 == 0.0f && dy0 == 0.0f && dx1 == 0.0f && dy1 == 0.0f) return y0 + (y1 - y0) * t;
					return NormalShaderCurveSegmentX(
						y0, dx0 / width, y0 + dy0, 1.0f + dx1 / width, y1 + dy1, y1, t
					);
				}
			} else if (type == 1.0f) {
				float y0 = float(curve.Anchors.front()[3]);
				for (size_t segment = 0; segment < segments; segment++) {
					if (x < float(curve.Anchors[segment][2])) return y0;
					y0 = float(curve.Anchors[segment][3]);
				}
				return y0;
			}
			// curve[amo - 3]: the last anchor's y.
			return float(curve.Anchors.back()[3]);
		};
		return minimum + (maximum - minimum) * evaluate();
	}
}
