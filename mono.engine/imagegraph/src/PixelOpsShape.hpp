#pragma once

// Signed distances for the four Shape variants observed in supplied projects.
// This private helper does not define the node's background or output surfaces.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::imagegraph::detail {
	enum class ShapeKind : uint8_t { Rectangle, Ellipse, Half, Triangle };
	enum class ShapeStatus : uint8_t { Ok, InvalidControl, UndefinedDivision };

	struct ShapePoint {
		double X = 0.0;
		double Y = 0.0;
	};

	struct ShapeGeometry {
		ShapeKind Kind = ShapeKind::Rectangle;
		ShapePoint Center{0.5, 0.5};
		ShapePoint HalfSize{0.5, 0.5};
		double RotationRadians = 0.0;
		ShapePoint Point1{};
		ShapePoint Point2{1.0, 1.0};
		ShapePoint Point3{1.0, 0.0};
	};

	inline double ShapeDot(ShapePoint a, ShapePoint b) {
		return a.X * b.X + a.Y * b.Y;
	}
	inline ShapePoint ShapeSubtract(ShapePoint a, ShapePoint b) {
		return {a.X - b.X, a.Y - b.Y};
	}
	inline double ShapeCross(ShapePoint a, ShapePoint b) {
		return a.X * b.Y - a.Y * b.X;
	}

	inline ShapeStatus ShapeDistance(
		const ShapeGeometry &geometry, uint32_t width, uint32_t height, double u, double v, double &distance
	) {
		const auto finitePoint = [](ShapePoint point) {
			return std::isfinite(point.X) && std::isfinite(point.Y);
		};
		if (width == 0 || height == 0 || !std::isfinite(u) || !std::isfinite(v) ||
			!finitePoint(geometry.Center) || !finitePoint(geometry.HalfSize) ||
			!finitePoint(geometry.Point1) || !finitePoint(geometry.Point2) || !finitePoint(geometry.Point3) ||
			!std::isfinite(geometry.RotationRadians) || geometry.Kind > ShapeKind::Triangle)
			return ShapeStatus::InvalidControl;
		if (geometry.HalfSize.X == 0.0 || geometry.HalfSize.Y == 0.0) return ShapeStatus::UndefinedDivision;
		const double cosine = std::cos(geometry.RotationRadians);
		const double sine = std::sin(geometry.RotationRadians);
		const double dx = u - geometry.Center.X;
		const double dy = v - geometry.Center.Y;
		const ShapePoint unscaled{dx * cosine - dy * sine, dx * sine + dy * cosine};
		const ShapePoint scaled{unscaled.X / geometry.HalfSize.X, unscaled.Y / geometry.HalfSize.Y};
		if (geometry.Kind == ShapeKind::Ellipse) {
			distance = std::hypot(scaled.X, scaled.Y) - 1.0;
		} else if (geometry.Kind == ShapeKind::Rectangle) {
			const double ratio = double(width) / height;
			const double qx = std::abs(unscaled.X * ratio) - geometry.HalfSize.X * ratio;
			const double qy = std::abs(unscaled.Y) - geometry.HalfSize.Y;
			distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0);
		} else if (geometry.Kind == ShapeKind::Half) {
			const double px = u - geometry.Point1.X / width;
			const double py = v - geometry.Point1.Y / height;
			// sdHalf receives negative rotation and a column-vector matrix.
			distance = -(std::sin(geometry.RotationRadians) * px + std::cos(geometry.RotationRadians) * py);
		} else {
			const auto mapPoint = [&](ShapePoint point) {
				return ShapePoint{
					(point.X / width - geometry.Center.X) * 2.0, (point.Y / height - geometry.Center.Y) * 2.0
				};
			};
			const std::array<ShapePoint, 3> points{
				mapPoint(geometry.Point1), mapPoint(geometry.Point2), mapPoint(geometry.Point3)
			};
			const ShapePoint edges[3]{
				ShapeSubtract(points[1], points[0]),
				ShapeSubtract(points[2], points[1]),
				ShapeSubtract(points[0], points[2])
			};
			const double winding = ShapeCross(edges[0], edges[2]);
			if (winding == 0.0) return ShapeStatus::UndefinedDivision;
			const double sign = winding < 0.0 ? -1.0 : 1.0;
			const ShapePoint pixel = scaled;
			double squared = std::numeric_limits<double>::infinity();
			double side = std::numeric_limits<double>::infinity();
			for (size_t i = 0; i < points.size(); i++) {
				const ShapePoint from = ShapeSubtract(pixel, points[i]);
				const double lengthSquared = ShapeDot(edges[i], edges[i]);
				if (lengthSquared == 0.0) return ShapeStatus::UndefinedDivision;
				const double t = std::clamp(ShapeDot(from, edges[i]) / lengthSquared, 0.0, 1.0);
				const ShapePoint closest{from.X - edges[i].X * t, from.Y - edges[i].Y * t};
				squared = std::min(squared, ShapeDot(closest, closest));
				side = std::min(side, sign * (from.X * edges[i].Y - from.Y * edges[i].X));
			}
			distance = -std::sqrt(squared) * (side < 0.0 ? -1.0 : side > 0.0 ? 1.0 : 0.0);
		}
		return std::isfinite(distance) ? ShapeStatus::Ok : ShapeStatus::UndefinedDivision;
	}
}
