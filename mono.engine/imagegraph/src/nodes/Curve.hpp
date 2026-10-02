#pragma once

// CPU curve evaluation from curve_bezier_function.gml eval_curve_x. GPU shaders use their own curve region,
// which differs in its step comparison and default-curve shortcuts; use this only where the source
// evaluates a curve in GML.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	inline double CurveLerp(double from, double to, double amount) {
		return from + (to - from) * amount;
	}

	inline double EvalCurveSegmentT(double y0, double ay0, double by1, double y1, double t) {
		const double i = 1.0 - t;
		return y0 * i * i * i + ay0 * 3.0 * i * i * t + by1 * 3.0 * i * t * t + y1 * t * t * t;
	}

	inline double EvalCurveSegmentX(
		double y0, double ax0, double ay0, double bx1, double by1, double y1, double x, double tolerance
	) {
		if (x <= 0.0) return y0;
		if (x >= 1.0) return y1;
		if (y0 == ay0 && y0 == by1 && y0 == y1) return y0;
		double t = x;
		for (int repeat = 0; repeat < 8; repeat++) {
			const double rest = 1.0 - t;
			const double ft = 3.0 * rest * rest * t * ax0 + 3.0 * rest * t * t * bx1 + t * t * t;
			if (std::abs(ft - x) < tolerance) return EvalCurveSegmentT(y0, ay0, by1, y1, t);
			const double slope =
				3.0 * rest * rest * ax0 + 6.0 * rest * t * (bx1 - ax0) + 3.0 * t * t * (1.0 - bx1);
			t = t - (ft - x) / slope;
		}
		return EvalCurveSegmentT(y0, ay0, by1, y1, t);
	}

	// Anchors after the six-number header, compared from the first anchor as array_equals_ext_fast does.
	inline bool CurveAnchorsEqual(const Curve &curve, const std::array<double, 12> &anchors) {
		if (curve.Anchors.size() != 2) return false;
		for (size_t index = 0; index < 12; index++)
			if (curve.Anchors[index / 6][index % 6] != anchors[index]) return false;
		return true;
	}

	inline double EvalCurveX(const Curve &curve, double x, double tolerance = 0.0001) {
		const double shift = curve.Header[0], scale = curve.Header[1], type = curve.Header[2];
		double minimum = curve.Header[3], maximum = curve.Header[4];
		if (minimum == 0.0 && maximum == 0.0) maximum = 1.0;
		x = std::clamp(x / scale - shift, 0.0, 1.0);
		constexpr double THIRD = 1.0 / 3.0;
		if (CurveAnchorsEqual(curve, {0, 0, 0, 1, THIRD, 0, -THIRD, 0, 1, 1, 0, 0}))
			return CurveLerp(minimum, maximum, 1.0);
		if (CurveAnchorsEqual(curve, {0, 0, 0, 0, THIRD, THIRD, -THIRD, -THIRD, 1, 1, 0, 0}))
			return CurveLerp(minimum, maximum, x);
		if (CurveAnchorsEqual(curve, {0, 0, 0, 1, THIRD, -THIRD, -THIRD, THIRD, 1, 0, 0, 0}))
			return CurveLerp(minimum, maximum, 1.0 - x);
		const size_t segments = curve.Anchors.empty() ? 0 : curve.Anchors.size() - 1;
		if (type == 0.0) {
			for (size_t segment = 0; segment < segments; segment++) {
				const auto &from = curve.Anchors[segment];
				const auto &to = curve.Anchors[segment + 1];
				const double x0 = from[2], y0 = from[3], x1 = to[2], y1 = to[3];
				if (x < x0 || x > x1) continue;
				double dx0 = from[4], dy0 = from[5], dx1 = to[0], dy1 = to[1];
				if (std::abs(dx0) + std::abs(dx1) > std::abs(x0 - x1) * 2.0) {
					const double ratio = (std::abs(x0 - x1) * 2.0) / (std::abs(dx0) + std::abs(dx1));
					dx0 *= ratio;
					dx1 *= ratio;
				}
				const double width = x1 - x0;
				const double t = (x - x0) / width;
				if (dx0 == 0.0 && dy0 == 0.0 && dx1 == 0.0 && dy1 == 0.0)
					return CurveLerp(minimum, maximum, CurveLerp(y0, y1, t));
				return CurveLerp(
					minimum,
					maximum,
					EvalCurveSegmentX(y0, dx0 / width, y0 + dy0, 1.0 + dx1 / width, y1 + dy1, y1, t, tolerance)
				);
			}
		} else if (type == 1.0) {
			double y0 = curve.Anchors.empty() ? 0.0 : curve.Anchors.front()[3];
			for (size_t segment = 0; segment < segments; segment++) {
				if (x <= curve.Anchors[segment][2]) return CurveLerp(minimum, maximum, y0);
				y0 = curve.Anchors[segment][3];
			}
			return CurveLerp(minimum, maximum, y0);
		}
		// array_safe_get_fast(_bz, length - 3): the last anchor's y.
		return CurveLerp(minimum, maximum, curve.Anchors.empty() ? 0.0 : curve.Anchors.back()[3]);
	}

	// The shader curve region's eval_curve_segment_x: Newton steps from t = x, tolerance 0.0001.
	inline double ShaderCurveSegmentX(double y0, double ax0, double ay0, double bx1, double by1, double y1, double x) {
		if (x <= 0.0) return y0;
		if (x >= 1.0) return y1;
		if (y0 == ay0 && y0 == by1 && y0 == y1) return y0;
		double t = x;
		for (int repeat = 0; repeat < 8; repeat++) {
			const double rest = 1.0 - t;
			const double ft = 3.0 * rest * rest * t * ax0 + 3.0 * rest * t * t * bx1 + t * t * t;
			if (std::abs(ft - x) < 0.0001) return EvalCurveSegmentT(y0, ay0, by1, y1, t);
			const double slope = 3.0 * rest * rest * ax0 + 6.0 * rest * t * (bx1 - ax0) + 3.0 * t * t * (1.0 - bx1);
			t = t - (ft - x) / slope;
		}
		return EvalCurveSegmentT(y0, ay0, by1, y1, t);
	}

	// curveEval from the shader curve region: the header's range wraps _curveEval, whose step mode uses a
	// strict comparison. GLSL holds 64 floats, so curves beyond nine anchors are undefined in the source.
	inline double EvalShaderCurve(const Curve &curve, double x) {
		double minimum = curve.Header[3], maximum = curve.Header[4];
		if (minimum == 0.0 && maximum == 0.0) maximum = 1.0;
		const auto evaluate = [&]() -> double {
			if (curve.Anchors.empty()) return 0.0;
			const size_t segments = curve.Anchors.size() - 1;
			const double type = curve.Header[2];
			x = std::clamp(x / curve.Header[1] - curve.Header[0], 0.0, 1.0);
			if (x <= curve.Anchors.front()[2]) return curve.Anchors.front()[3];
			if (x >= curve.Anchors.back()[2]) return curve.Anchors.back()[3];
			if (type == 0.0) {
				for (size_t segment = 0; segment < segments; segment++) {
					const auto &from = curve.Anchors[segment];
					const auto &to = curve.Anchors[segment + 1];
					const double x0 = from[2], y0 = from[3], x1 = to[2], y1 = to[3];
					if (x < x0 || x > x1) continue;
					double dx0 = from[4], dy0 = from[5], dx1 = to[0], dy1 = to[1];
					if (std::abs(dx0) + std::abs(dx1) > std::abs(x0 - x1) * 2.0) {
						const double ratio = (std::abs(x0 - x1) * 2.0) / (std::abs(dx0) + std::abs(dx1));
						dx0 *= ratio;
						dx1 *= ratio;
					}
					const double width = x1 - x0;
					const double t = (x - x0) / width;
					if (dx0 == 0.0 && dy0 == 0.0 && dx1 == 0.0 && dy1 == 0.0) return y0 + (y1 - y0) * t;
					return ShaderCurveSegmentX(y0, dx0 / width, y0 + dy0, 1.0 + dx1 / width, y1 + dy1, y1, t);
				}
			} else if (type == 1.0) {
				double y0 = curve.Anchors.front()[3];
				for (size_t segment = 0; segment < segments; segment++) {
					if (x < curve.Anchors[segment][2]) return y0;
					y0 = curve.Anchors[segment][3];
				}
				return y0;
			}
			// curve[amo - 3]: the last anchor's y.
			return curve.Anchors.back()[3];
		};
		return CurveLerp(minimum, maximum, evaluate());
	}
}
