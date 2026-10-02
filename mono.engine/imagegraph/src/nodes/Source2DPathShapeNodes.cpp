#include "../NodeExecutors.hpp"
#include "../SourcePathShape.hpp"
#include "Curve.hpp"

#include <array>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		using Kind = SourcePathShapeKind2D;

		Vector2 RotatePoint(Vector2 point, Vector2 center, double degrees) {
			return RotateSourceShapePoint(point, center, degrees);
		}
		Vector2 Radial(Vector2 center, Vector2 radius, double degrees) {
			const double angle = degrees * std::numbers::pi / 180;
			return {
				center.X + SourceShapeLengthdirComponent(radius.X * std::cos(angle)),
				center.Y + SourceShapeLengthdirComponent(-radius.Y * std::sin(angle))
			};
		}
		bool
		Corner(std::vector<Vector2> &points, Vector2 before, Vector2 center, Vector2 after, double radius) {
			if (radius == 0) {
				points.push_back(center);
				return true;
			}
			const double angle0 =
							 std::atan2(center.Y - before.Y, before.X - center.X) * 180 / std::numbers::pi,
						 angle1 = std::atan2(center.Y - after.Y, after.X - center.X) * 180 / std::numbers::pi;
			const Vector2 p1 = Radial(center, {radius, radius}, angle0),
						  p3 = Radial(center, {radius, radius}, angle1), p2 = RotatePoint(center, p1, -90),
						  p4 = RotatePoint(center, p3, 90);
			const double denominator = (p1.X - p2.X) * (p3.Y - p4.Y) - (p1.Y - p2.Y) * (p3.X - p4.X);
			if (denominator == 0) return true;
			const double first = p1.X * p2.Y - p1.Y * p2.X, second = p3.X * p4.Y - p3.Y * p4.X;
			const Vector2 origin{
				(first * (p3.X - p4.X) - (p1.X - p2.X) * second) / denominator,
				(first * (p3.Y - p4.Y) - (p1.Y - p2.Y) * second) / denominator
			};
			const double r = std::hypot(origin.X - p1.X, origin.Y - p1.Y),
						 start = std::atan2(origin.Y - p1.Y, p1.X - origin.X) * 180 / std::numbers::pi,
						 end = std::atan2(origin.Y - p3.Y, p3.X - origin.X) * 180 / std::numbers::pi;
			double difference = end - start;
			difference -= std::floor((difference + 180) / 360) * 360;
			for (size_t i = 0; i <= 64; ++i)
				points.push_back(Radial(origin, {r, r}, start + difference * double(i) / 64));
			return true;
		}
	}
	bool SourcePathShape(NodeContext &context) {
		const std::string shapeName = context.Get<std::string>("shape", "Rectangle");
		const auto found = std::find(SourcePathShapeNames.begin(), SourcePathShapeNames.end(), shapeName);
		if (found == SourcePathShapeNames.end())
			return context.Fail(Status::InvalidValue, "Shape Path type is unknown", "shape");
		SourcePathShapeData2D shape;
		shape.Kind = Kind(found - SourcePathShapeNames.begin());
		const auto vector = [&](std::string_view port, Vector2 fallback) {
			Vector2 result = context.Vec2(port, fallback);
			if (!context.IsLinked(port) && context.Integer(std::string(port) + "_unit", 1) == 1) {
				result.X *= context.Project.SurfaceWidth;
				result.Y *= context.Project.SurfaceHeight;
			}
			return result;
		};
		shape.Position = vector("position", {.5, .5});
		shape.HalfSize = vector("half_size", {.5, .5});
		shape.Rotation = context.Scalar("rotation");
		shape.AngleRange = context.Vec2("angle_range", {0, 90});
		shape.Factor = context.Scalar("factor", 4);
		const Vector2 pos = shape.Position, size = shape.HalfSize;
		const double skew = context.Scalar("skew", .5), inner = context.Scalar("inner_radius", .5),
					 revolution = context.Scalar("revolution", 4), angle = context.Scalar("angle"),
					 pitch = context.Scalar("pitch", .2);
		const int64_t sides = context.Integer("sides", 4), resolution = context.Integer("resolution", 64);
		const auto append = [&](Vector2 point) {
			if (!std::isfinite(point.X) || !std::isfinite(point.Y))
				return context.Fail(
					Status::UnsupportedExecution,
					"Shape Path source geometry is undefined or nonfinite",
					"shape"
				);
			if (shape.Points.size() >= Limits::MaximumPathAnchors)
				return context.Fail(
					Status::LimitExceeded, "Shape Path point count exceeds source bounds", "resolution"
				);
			shape.Points.push_back(point);
			return true;
		};
		const auto count = [&](double amount) -> std::optional<size_t> {
			if (!std::isfinite(amount) || amount < 0 || amount > Limits::MaximumPathAnchors) {
				context.Fail(
					Status::LimitExceeded, "Shape Path source point count exceeds bounds", "resolution"
				);
				return {};
			}
			if (amount != std::trunc(amount)) {
				context.Fail(
					Status::UnsupportedExecution,
					"Shape Path fractional array size needs source runner index observations",
					"resolution"
				);
				return {};
			}
			return size_t(amount);
		};
		double capacity = 64;
		switch (shape.Kind) {
		case Kind::Rectangle:
		case Kind::Trapezoid:
		case Kind::Parallelogram: {
			const Vector4 radii = context.Get<Vector4>("corner_radius", {});
			capacity = (radii.X == 0 ? 1 : 65) + (radii.Y == 0 ? 1 : 65) + (radii.Z == 0 ? 1 : 65) +
					   (radii.W == 0 ? 1 : 65);
			break;
		}
		case Kind::Polygon:
		case Kind::StarDraw:
			capacity = double(sides);
			break;
		case Kind::Star:
			capacity = double(sides) * 2;
			break;
		case Kind::Hypocycloid:
			capacity = 32 * shape.Factor * revolution;
			break;
		case Kind::Epitrochoid:
			capacity = 64 * revolution / (1 / std::max(.1, inner));
			break;
		case Kind::Twist:
			capacity = 2 * (64 / std::clamp(std::max(shape.Factor, 0.), .1, 1.)) + 1;
			break;
		case Kind::Line:
			capacity = 2;
			break;
		case Kind::Spiral:
		case Kind::SpiralCircle:
			capacity = resolution * std::abs(revolution);
			break;
		default:
			break;
		}
		const auto admittedCount = count(capacity);
		if (!admittedCount) return false;
		auto scratch = context.ReserveWorkspace(*admittedCount * sizeof(Vector2), "path_data");
		if (!scratch) return false;
		shape.Points.reserve(*admittedCount);
		const auto radial = [&](double degrees, double radius = 1.) {
			return Radial(pos, {size.X * radius, size.Y * radius}, degrees);
		};
		const bool pitchCurved = context.Boolean("pitch_curved") &&
								 (shape.Kind == Kind::Spiral || shape.Kind == Kind::SpiralCircle);
		std::array<double, 33> pitchSamples{};
		if (pitchCurved) {
			const Value *value = context.Find("pitch_curve");
			const Curve *curve = value ? std::get_if<Curve>(value) : nullptr;
			if (!curve)
				return context.Fail(
					Status::InvalidValue, "Shape Path requires its enabled pitch curve", "pitch_curve"
				);
			for (size_t i = 0; i < pitchSamples.size(); ++i)
				pitchSamples[i] = EvalCurveX(*curve, double(i) / 32, .00001);
		}
		const auto curveMap = [&](double ratio) {
			if (std::isnan(ratio)) return 0.;
			const double coordinate = std::clamp(ratio, 0., 1.) * 32;
			const size_t low = size_t(std::floor(coordinate)), high = size_t(std::ceil(coordinate));
			return pitchSamples[low] + (pitchSamples[high] - pitchSamples[low]) * (coordinate - low);
		};
		if (shape.Kind == Kind::Rectangle || shape.Kind == Kind::Trapezoid ||
			shape.Kind == Kind::Parallelogram) {
			const double narrow = std::clamp(1 - skew, 0., 1.), wide = std::clamp(1 + skew, 0., 1.);
			std::array<Vector2, 4> corners{
				{{pos.X - size.X, pos.Y - size.Y},
				 {pos.X + size.X, pos.Y - size.Y},
				 {pos.X + size.X, pos.Y + size.Y},
				 {pos.X - size.X, pos.Y + size.Y}}
			};
			if (shape.Kind == Kind::Trapezoid)
				corners = {
					{{pos.X - size.X * narrow, pos.Y - size.Y},
					 {pos.X + size.X * narrow, pos.Y - size.Y},
					 {pos.X + size.X * wide, pos.Y + size.Y},
					 {pos.X - size.X * wide, pos.Y + size.Y}}
				};
			if (shape.Kind == Kind::Parallelogram)
				corners = {
					{{pos.X - size.X * narrow, pos.Y - size.Y},
					 {pos.X + size.X * wide, pos.Y - size.Y},
					 {pos.X + size.X * narrow, pos.Y + size.Y},
					 {pos.X - size.X * wide, pos.Y + size.Y}}
				};
			const Vector4 raw = context.Get<Vector4>("corner_radius", {});
			std::array<double, 4> radii{raw.X, raw.Y, raw.W, raw.Z};
			if (!context.IsLinked("corner_radius") && context.Integer("corner_radius_unit", 1) == 1) {
				radii[0] *= context.Project.SurfaceWidth;
				radii[1] *= context.Project.SurfaceHeight;
				radii[2] *= context.Project.SurfaceHeight;
				radii[3] *= context.Project.SurfaceWidth;
			}
			for (size_t i = 0; i < 4; ++i) {
				Vector2 before = corners[(i + 3) % 4];
				// The fourth Trapezoid corner uses x2 rather than x1 in its source previous point.
				if (i == 3 && shape.Kind == Kind::Trapezoid) before.X = corners[2].X;
				if (!Corner(shape.Points, before, corners[i], corners[(i + 1) % 4], radii[i])) return false;
			}
		} else if (shape.Kind == Kind::Ellipse || shape.Kind == Kind::Arc || shape.Kind == Kind::Squircle) {
			shape.Loop = shape.Kind != Kind::Arc;
			for (size_t i = 0; i < 64; ++i) {
				const double a =
					shape.Kind == Kind::Arc
						? shape.AngleRange.X + (shape.AngleRange.Y - shape.AngleRange.X) * double(i) / 63
						: double(i) * 360 / 64;
				double radius = 1;
				if (shape.Kind == Kind::Squircle) {
					const double factor = std::max(shape.Factor, 0.),
								 quadrant = std::fmod(a, 90) * std::numbers::pi / 180;
					if (factor == 0)
						return context.Fail(
							Status::UnsupportedExecution, "Squircle source exponent divides by zero", "factor"
						);
					radius =
						1 / std::pow(
								std::pow(std::cos(quadrant), factor) + std::pow(std::sin(quadrant), factor),
								1 / factor
							);
				}
				if (!append(radial(a, radius))) return false;
			}
		} else if (shape.Kind == Kind::Polygon || shape.Kind == Kind::Star || shape.Kind == Kind::StarDraw) {
			const auto steps = count(double(sides) * (shape.Kind == Kind::Star ? 2 : 1));
			if (!steps) return false;
			if (sides == 0)
				return context.Fail(
					Status::UnsupportedExecution, "Shape Path angle division is undefined", "sides"
				);
			for (int64_t i = 0; i < sides; ++i) {
				const double a = 360. * i / sides * (shape.Kind == Kind::StarDraw ? 2 : 1);
				if (!append(radial(a))) return false;
				if (shape.Kind == Kind::Star && !append(radial(a + 180. / sides, inner))) return false;
			}
		} else if (shape.Kind == Kind::Hypocycloid || shape.Kind == Kind::Epitrochoid) {
			const double innerM = 1 / std::max(.1, inner), rawSteps = shape.Kind == Kind::Hypocycloid
																		  ? 32 * shape.Factor * revolution
																		  : 64 * revolution / innerM;
			const auto steps = count(rawSteps);
			if (!steps) return false;
			if (rawSteps == 1 ||
				(shape.Kind == Kind::Hypocycloid ? shape.Factor == 0 : 1 + shape.Factor == 0))
				return context.Fail(
					Status::UnsupportedExecution, "Shape Path source curve division is undefined", "factor"
				);
			for (size_t i = 0; i < *steps; ++i) {
				const double a = (360. / (rawSteps - 1) * revolution) * i * std::numbers::pi / 180;
				Vector2 point;
				if (shape.Kind == Kind::Hypocycloid)
					point = {
						pos.X + ((shape.Factor - 1) * std::cos(a) + std::cos((shape.Factor - 1) * a)) *
									size.X / shape.Factor,
						pos.Y + ((shape.Factor - 1) * std::sin(a) - std::sin((shape.Factor - 1) * a)) *
									size.Y / shape.Factor
					};
				else
					point = {
						pos.X + ((1 + innerM) * std::cos(a) -
								 shape.Factor * std::cos((1 + innerM) / innerM * a)) *
									size.X / (1 + shape.Factor),
						pos.Y + ((1 + innerM) * std::sin(a) -
								 shape.Factor * std::sin((1 + innerM) / innerM * a)) *
									size.Y / (1 + shape.Factor)
					};
				if (!append(point)) return false;
			}
		} else if (shape.Kind == Kind::Twist) {
			const double factor = std::max(shape.Factor, 0.), rawSteps = 64 / std::clamp(factor, .1, 1.);
			const auto steps = count(rawSteps * 2 + 1);
			if (!steps) return false;
			shape.Points.resize(*steps);
			for (size_t i = 0; i <= size_t(rawSteps); ++i) {
				double x = double(i) / rawSteps * 2 - 1;
				x = (x < 0 ? -1 : x > 0 ? 1 : 0) * std::pow(std::abs(x), 1 / (factor + 1));
				const double target = (x < 0   ? -1
									   : x > 0 ? 1
											   : 0) *
									  std::sqrt(1 - x * x),
							 y = x + (target - x) * std::pow(std::abs(x), factor);
				shape.Points[i] = {pos.X + x * size.X, pos.Y + y * size.Y};
				shape.Points[size_t(rawSteps * 2) - i] = {pos.X + x * size.X, pos.Y - y * size.Y};
			}
		} else if (shape.Kind == Kind::Line) {
			shape.Loop = false;
			if (!append({pos.X - size.X, pos.Y}) || !append({pos.X + size.X, pos.Y})) return false;
		} else if (shape.Kind == Kind::Curve) {
			shape.Loop = false;
			const int64_t equation = context.Integer("curve_eq");
			const Value *curveValue = context.Find("custom_curve");
			const Curve *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
			if (equation == 2 && !curve)
				return context.Fail(
					Status::InvalidValue, "Shape Path requires its custom curve", "custom_curve"
				);
			for (size_t i = 0; i < 64; ++i) {
				const double ratio = double(i) / 63;
				const double y =
					equation == 0 ? pos.Y + std::sin((180. / 63 * i) * std::numbers::pi / 180) * shape.Factor
					: equation == 1 ? pos.Y + std::sqrt(1 - std::pow(2 * (ratio - .5), 2)) * shape.Factor
									: pos.Y - EvalCurveX(*curve, ratio) * shape.Factor;
				if (!append({pos.X + size.X * (2 * ratio - 1), y})) return false;
			}
		} else {
			shape.Loop = false;
			const double rawSteps = resolution * std::abs(revolution);
			const auto steps = count(rawSteps);
			if (!steps) return false;
			if (resolution == 0 || rawSteps == 1 ||
				(shape.Kind == Kind::Spiral && context.Boolean("fixed_pitch") && revolution == 0))
				return context.Fail(
					Status::UnsupportedExecution,
					"Shape Path source spiral division is undefined",
					"resolution"
				);
			double effectivePitch = pitch, effectiveAngle = angle;
			const double angleStep = 360. / resolution * (revolution < 0 ? -1 : revolution > 0 ? 1 : 0);
			if (shape.Kind == Kind::Spiral) {
				if (context.Boolean("fixed_pitch")) effectivePitch /= revolution;
				if (context.Boolean("reverse")) {
					effectivePitch = 1 / std::abs(revolution);
					effectiveAngle -= angleStep * rawSteps;
				}
			} else
				effectivePitch = std::max(pitch, .01);
			for (size_t i = 0; i < *steps; ++i) {
				double radius;
				if (shape.Kind == Kind::Spiral)
					radius = pitchCurved ? revolution * effectivePitch * curveMap(double(i) / (rawSteps - 1))
										 : effectivePitch / resolution * i;
				else
					radius = pitchCurved ? (1 - double(i) / rawSteps) * curveMap(double(i) / (rawSteps - 1))
										 : std::sqrt(1 - std::pow(double(i) / rawSteps, effectivePitch * 10));
				if (!append(radial(effectiveAngle + angleStep * i, radius))) return false;
			}
		}
		for (auto &point : shape.Points) {
			point = RotatePoint(point, pos, shape.Rotation);
			if (!std::isfinite(point.X) || !std::isfinite(point.Y))
				return context.Fail(
					Status::UnsupportedExecution, "Shape Path rotation is undefined", "rotation"
				);
		}
		if (!context.ReserveOutput(
				sizeof(SourcePathData2D) + shape.Points.capacity() * sizeof(Vector2) + 32, "path_data"
			))
			return false;
		Path2D path;
		auto &operation = path.SourceOperation.emplace();
		operation.Kind = SourcePathOperationKind::Shape;
		operation.Shape = std::move(shape);
		context.SetValue("path_data", std::move(path));
		return context.FailureCode == Status::Ok;
	}
}
