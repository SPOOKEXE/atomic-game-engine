#include <engine/imagegraph/FrameTime.hpp>
// CPU curve construction and sampling from the pinned Curve Fn and Evaluate Curve scripts.
#include "Curve.hpp"
#include "Families.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace engine::imagegraph::detail {
	namespace {
		bool CurveFunction(NodeContext &context) {
			const double mode = context.SourceChoice("type");
			if (context.FailureCode != Status::Ok) return false;
			if (mode < 0 || mode > 4 || std::trunc(mode) != mode)
				return context.Fail(
					Status::UnsupportedExecution, "curve function mode is unresolved", "type"
				);
			const Vector2 range = context.Vec2("range", {0, 1});
			const Vector2 output = context.Vec2("output_range", {0, 1});
			const double shift = context.Scalar("shift"), scale = context.Scalar("scale", 1);
			const double frequency = context.Scalar("frequency", 1), phase = context.Scalar("phase");
			if (!std::isfinite(range.X) || !std::isfinite(range.Y))
				return context.Fail(Status::InvalidValue, "curve range must be finite", "range");
			if (!std::isfinite(output.X) || !std::isfinite(output.Y))
				return context.Fail(
					Status::InvalidValue, "curve output range must be finite", "output_range"
				);
			if (!std::isfinite(shift))
				return context.Fail(Status::InvalidValue, "curve shift must be finite", "shift");
			if (!std::isfinite(scale) || scale == 0)
				return context.Fail(Status::InvalidValue, "curve scale must be finite and nonzero", "scale");
			if (mode > 0 && mode < 4 && (!std::isfinite(frequency) || !std::isfinite(phase)))
				return context.Fail(
					Status::InvalidValue,
					"curve frequency and phase must be finite",
					!std::isfinite(frequency) ? "frequency" : "phase"
				);
			const char *countPort = mode == 4 ? "step" : "resolution";
			const int64_t intervals = mode == 0 ? 1 : context.Integer(countPort, mode == 4 ? 4 : 0);
			if (context.FailureCode != Status::Ok) return false;
			if (intervals < (mode == 4 ? 2 : 1))
				return context.Fail(
					Status::InvalidValue, "curve sampling requires a nonzero finite denominator", countPort
				);
			if (uint64_t(intervals) >= Limits::MaximumCurveAnchors)
				return context.Fail(
					Status::LimitExceeded, "curve sampling exceeds the anchor limit", countPort
				);
			const size_t count = static_cast<size_t>(intervals) + 1;
			if (!context.ReserveOutput(
					count * sizeof(std::array<double, 6>) + std::string{}.capacity(), "curve"
				))
				return false;
			Curve curve{{shift, scale, mode == 4 ? 1.0 : 0.0, output.X, output.Y, 0}, {}};
			curve.Anchors.reserve(count);
			// The source reads Y Range but never applies it in Curve Fn.
			for (int64_t index = 0; index <= intervals; index++) {
				const double x = static_cast<double>(index) / static_cast<double>(intervals);
				double ratio = x;
				if (mode > 0 && mode < 4) {
					const double t = x * frequency - phase;
					if (!std::isfinite(t))
						return context.Fail(
							Status::InvalidValue, "curve phase exceeded finite range", "frequency"
						);
					if (mode == 1) {
						const double angle = t * std::numbers::pi * 2;
						if (!std::isfinite(angle))
							return context.Fail(
								Status::InvalidValue, "curve angle exceeded finite range", "frequency"
							);
						ratio = std::sin(angle) * .5 + .5;
					} else {
						// Preserve the source nested frac expression, including negative phase offsets.
						const double positive = std::fmod(std::fmod(t, 1.0) + 1.0, 1.0);
						ratio = mode == 2 ? std::abs(positive * 2 - 1) : positive;
					}
				} else if (mode == 4)
					ratio =
						std::clamp(static_cast<double>(index) / static_cast<double>(intervals - 1), 0.0, 1.0);
				const double y = CurveLerp(range.X, range.Y, ratio);
				if (!std::isfinite(y))
					return context.Fail(Status::InvalidValue, "curve anchor exceeded finite range", "range");
				curve.Anchors.push_back({0, 0, x, y, 0, 0});
			}
			context.SetValue("curve", std::move(curve));
			return true;
		}

		bool EvaluateCurve(NodeContext &context) {
			const Value *value = context.Find("curve");
			const Curve *curve = value ? std::get_if<Curve>(value) : nullptr;
			if (!curve || !ValidRuntimeValue(*value))
				return context.Fail(
					Status::InvalidValue, "curve sampling requires a bounded finite curve", "curve"
				);
			if (curve->Header[1] == 0 || (curve->Header[2] != 0 && curve->Header[2] != 1))
				return context.Fail(
					Status::InvalidValue, "curve scale or interpolation mode is invalid", "curve"
				);
			const double display = context.SourceChoice("display_type");
			if (context.FailureCode != Status::Ok) return false;
			if (display != 0 && display != 1)
				return context.Fail(
					Status::UnsupportedExecution, "curve display mode is unresolved", "display_type"
				);
			double progress = context.Scalar("progress");
			if (context.Boolean("animated")) {
				if (!context.Timeline || context.Timeline->Frames <= 1)
					return context.Fail(
						Status::InvalidValue,
						"animated curve requires at least two timeline frames",
						"animated"
					);
				// The source samples the signed authoring clock, including Control/Alt seeks.
				progress = static_cast<double>(FrameTimeToReal(GetFrameTime(context.Request))) /
						   static_cast<double>(context.Timeline->Frames - 1);
			}
			const double minimum = context.Scalar("minimum"), maximum = context.Scalar("maximum", 1);
			if (!std::isfinite(progress) || !std::isfinite(minimum) || !std::isfinite(maximum))
				return context.Fail(
					Status::InvalidValue,
					"curve sample controls must be finite",
					!std::isfinite(progress)  ? "progress"
					: !std::isfinite(minimum) ? "minimum"
											  : "maximum"
				);
			const double output = CurveLerp(minimum, maximum, EvalCurveX(*curve, progress));
			if (!std::isfinite(output))
				return context.Fail(Status::InvalidValue, "curve sample exceeded finite range", "curve");
			if (!context.ReserveOutput(std::string{}.capacity(), "curve")) return false;
			context.SetValue("curve", output);
			return true;
		}
	}
	std::span<const ExecutorEntry> CurveExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.curve_function", CurveFunction}, ExecutorEntry{"pc.anim_curve", EvaluateCurve}
		};
		return ENTRIES;
	}
}
