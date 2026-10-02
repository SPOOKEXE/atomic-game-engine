#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>

namespace engine::imagegraph::detail {
	inline constexpr std::array<std::string_view, 16> SourcePathShapeNames{
		"Rectangle",
		"Trapezoid",
		"Parallelogram",
		"Ellipse",
		"Arc",
		"Squircle",
		"Hypocycloid",
		"Epitrochoid",
		"Polygon",
		"Star",
		"Star Draw",
		"Twist",
		"Line",
		"Curve",
		"Spiral",
		"Spiral Circle"
	};
	inline bool ValidSourcePathShape(const SourcePathShapeData2D &shape) {
		if (size_t(shape.Kind) >= SourcePathShapeNames.size() ||
			shape.Points.size() > Limits::MaximumPathAnchors)
			return false;
		const auto finite = [](Vector2 point) { return std::isfinite(point.X) && std::isfinite(point.Y); };
		if (!finite(shape.Position) || !finite(shape.HalfSize) || !finite(shape.AngleRange) ||
			!std::isfinite(shape.Rotation) || !std::isfinite(shape.Factor))
			return false;
		return std::all_of(shape.Points.begin(), shape.Points.end(), finite);
	}
	// lengthdir snaps the component before the caller adds the source position.
	inline double SourceShapeLengthdirComponent(double component) {
		const double nearest = std::round(component);
		return std::abs(component - nearest) < .0001 ? nearest : component;
	}
	inline Vector2 RotateSourceShapePoint(Vector2 point, Vector2 origin, double degrees) {
		if (degrees == 0) return point;
		if (degrees == 180) return {origin.X + (origin.X - point.X), origin.Y + (origin.Y - point.Y)};
		const double angle = -degrees * std::numbers::pi / 180;
		const double x = point.X - origin.X, y = point.Y - origin.Y;
		return {
			origin.X + x * std::cos(angle) - y * std::sin(angle),
			origin.Y + x * std::sin(angle) + y * std::cos(angle)
		};
	}
	inline std::optional<Vector2> SourceShapeDistance(
		const SourcePathShapeData2D &shape, std::span<const double> lengths, double total, double distance
	) {
		if (!std::isfinite(distance) || !std::isfinite(total)) return std::nullopt;
		if (total == 0) return Vector2{};
		if (distance < 0) distance = total + std::fmod(distance, total);
		if (shape.Loop) {
			distance = std::fmod(distance, total);
			if (distance < 0) distance += total;
		} else {
			// GML clamp keeps the source upper endpoint even when a very short path
			// makes it negative.
			distance = std::min(std::max(distance, 0.), total - .1);
		}
		for (size_t index = 0; index < lengths.size(); ++index) {
			const double length = lengths[index];
			if (length == 0) continue;
			if (distance > length) {
				distance -= length;
				continue;
			}
			if (shape.Points.empty()) return std::nullopt;
			const auto first = shape.Points[index % shape.Points.size()];
			const auto next = shape.Points[(index + 1) % shape.Points.size()];
			const double ratio = distance / length;
			const Vector2 point{first.X + (next.X - first.X) * ratio, first.Y + (next.Y - first.Y) * ratio};
			return std::isfinite(point.X) && std::isfinite(point.Y) ? std::optional{point} : std::nullopt;
		}
		return Vector2{};
	}
	inline std::optional<Vector2> SourceShapeRatio(
		const SourcePathShapeData2D &shape, std::span<const double> lengths, double total, double ratio
	) {
		if (!std::isfinite(ratio)) return std::nullopt;
		if (ratio < 0) ratio = 1 + ratio - std::trunc(ratio);
		ratio = shape.Loop ? ratio - std::trunc(ratio) : std::clamp(ratio, 0., .999);
		double angle = 360 * ratio, radius = 1;
		if (shape.Kind == SourcePathShapeKind2D::Arc)
			angle = shape.AngleRange.X + (shape.AngleRange.Y - shape.AngleRange.X) * ratio;
		else if (shape.Kind == SourcePathShapeKind2D::Squircle) {
			const double quadrant = std::fmod(angle, 90.) * std::numbers::pi / 180;
			if (shape.Factor == 0) return std::nullopt;
			radius = 1 / std::pow(
							 std::pow(std::cos(quadrant), shape.Factor) +
								 std::pow(std::sin(quadrant), shape.Factor),
							 1 / shape.Factor
						 );
		} else if (shape.Kind != SourcePathShapeKind2D::Ellipse)
			return SourceShapeDistance(shape, lengths, total, ratio * total);
		const double radians = angle * std::numbers::pi / 180;
		const Vector2 point = RotateSourceShapePoint(
			{shape.Position.X + SourceShapeLengthdirComponent(shape.HalfSize.X * radius * std::cos(radians)),
			 shape.Position.Y +
				 SourceShapeLengthdirComponent(-shape.HalfSize.Y * radius * std::sin(radians))},
			shape.Position,
			shape.Rotation
		);
		return std::isfinite(point.X) && std::isfinite(point.Y) ? std::optional{point} : std::nullopt;
	}
} // namespace engine::imagegraph::detail
