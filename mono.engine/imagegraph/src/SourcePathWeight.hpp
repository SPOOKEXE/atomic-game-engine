#pragma once

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <span>

namespace engine::imagegraph::detail {
	class NodeContext;
	std::optional<uint64_t> SourceWeightRuntimeWork(const Path2D *planar, const PathData3D *spatial);
	// Owns the spatial runtime without creating a Path.hpp / Path3D.hpp include
	// cycle.
	class SourcePathWeightRuntime3D {
		struct Storage;
		AllocationReservation Charge;
		std::unique_ptr<Storage> Data;

	  public:
		SourcePathWeightRuntime3D(NodeContext &, const PathData3D &);
		~SourcePathWeightRuntime3D();
		SourcePathWeightRuntime3D(SourcePathWeightRuntime3D &&) noexcept;
		SourcePathWeightRuntime3D &operator=(SourcePathWeightRuntime3D &&) noexcept;
		bool Valid() const;
		std::array<double, 3> Ratio(double ratio, size_t line) const;
		SourcePathPointBuffer RatioInto(double ratio, size_t line, SourcePathPointBuffer &out) const;
		SourcePathPointBuffer DistanceInto(double distance, size_t line, SourcePathPointBuffer &out) const;
		size_t OriginalChildCount() const;
		size_t OriginalChildLineCount(size_t child) const;
		double OriginalChildLength(size_t child) const;
		size_t OriginalChildSegmentCount(size_t child) const;
		SourcePathPointBuffer OriginalChildDistanceInto(
			size_t child, double distance, size_t line, SourcePathPointBuffer &out
		) const;
		size_t LineCount() const;
		double Length(size_t line) const;
		size_t SegmentCount(size_t line) const;
		size_t AccumulatedCount(size_t line) const;
		double AccumulatedAt(size_t index, size_t line) const;
		std::optional<Vector4> Boundary(size_t line = 0) const;
	};
	// Official HTML5 point_direction rounds its nonvertical atan2 result to six
	// decimals.
	inline double SourceWeightDirection(double x, double y) {
		if (x == 0) return y > 0 ? 270. : y < 0 ? 90. : 0.;
		const double angle = std::atan2(y, x) * 180 / std::numbers::pi;
		const double scaled = angle * 1000000, base = std::floor(scaled), fraction = scaled - base;
		const double rounded =
			(base + (fraction > .5 || (fraction == .5 && std::fmod(base, 2) != 0))) / 1000000;
		return rounded <= 0 ? -rounded : 360 - rounded;
	}
	inline double SourceWeightCurve(std::span<const double> table, double ratio) {
		if (std::isnan(ratio)) return 0;
		const double position = std::clamp(ratio, 0., 1.) * double(table.size() - 1);
		const auto lo = size_t(std::floor(position)), hi = size_t(std::ceil(position));
		return table[lo] + (table[hi] - table[lo]) * (position - std::floor(position));
	}
} // namespace engine::imagegraph::detail
