#pragma once
#include "SourcePathShape.hpp"
#include "SourceVerletPath.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
namespace engine::imagegraph::detail {
	inline bool ValidSourcePath2D(const Path2D &path, size_t depth = 0, size_t *count = nullptr) {
		size_t local = 0;
		if (!count) count = &local;
		if (depth > Limits::MaximumArrayDepth || *count >= Limits::MaximumArrayElements) return false;
		++*count;
		if (path.Anchors.size() > Limits::MaximumPathAnchors ||
			path.Weights.size() > Limits::MaximumPathWeights)
			return false;
		for (const auto &anchor : path.Anchors)
			for (double value : anchor.Controls)
				if (!std::isfinite(value)) return false;
		for (const auto &weight : path.Weights)
			if (!std::isfinite(weight.Position) || !std::isfinite(weight.Weight)) return false;
		if (!path.SourceOperation) return true;
		if (path.Loop || path.Segmented || !path.Anchors.empty() || !path.Weights.empty()) return false;
		const auto &op = *path.SourceOperation;
		if (op.Shape && op.Kind != SourcePathOperationKind::Shape) return false;
		if (op.Kind == SourcePathOperationKind::Redistribute) {
			if (!op.RedistributeMap || op.Inputs.size() != 1 || 33 > Limits::MaximumArrayElements - *count)
				return false;
			*count += 33;
			for (double value : *op.RedistributeMap)
				if (!std::isfinite(value)) return false;
		} else if (op.RedistributeMap)
			return false;
		if (!std::isfinite(op.SkewCenter.X) || !std::isfinite(op.SkewCenter.Y) ||
			!std::isfinite(op.SkewStrength))
			return false;
		if (op.Kind == SourcePathOperationKind::Skew) {
			if (op.SkewAxis > 1 || op.Inputs.size() != 1) return false;
		} else if (op.SkewAxis != 0 || op.SkewStrength != 0 || op.SkewCenter != Vector2{})
			return false;

		if (!std::isfinite(op.Offset) || !std::isfinite(op.BlendAmount)) return false;
		if (op.Kind != SourcePathOperationKind::Offset && (op.Offset != 0 || op.ClampOffset)) return false;
		if (op.Kind != SourcePathOperationKind::Blend &&
			(op.BlendAmount != 0 || op.BlendMode != 0 ||
			 op.BlendInputsValid != std::array<bool, 2>{false, false}))
			return false;
		if (op.Kind != SourcePathOperationKind::Join && !op.Reversed.empty()) return false;
		if (op.Kind != SourcePathOperationKind::Blend &&
			(!op.BlendLengths.empty() || !op.BlendAccumulated.empty()))
			return false;
		if (op.BlendLengths.size() != op.BlendAccumulated.size()) return false;
		size_t cached = op.BlendLengths.size();
		if (cached > Limits::MaximumArrayElements) return false;
		for (double length : op.BlendLengths)
			if (!std::isfinite(length)) return false;
		for (const auto &row : op.BlendAccumulated) {
			if (row.size() > Limits::MaximumArrayElements - cached) return false;
			cached += row.size();
			for (double length : row)
				if (!std::isfinite(length)) return false;
		}
		if (cached > Limits::MaximumArrayElements - *count) return false;
		*count += cached;

		if (op.Kind == SourcePathOperationKind::Offset && op.Inputs.size() > 1) return false;
		if (op.Kind == SourcePathOperationKind::Blend && (op.Inputs.size() != 2 || op.BlendMode > 3))
			return false;
		if (op.Kind == SourcePathOperationKind::Join &&
			(op.Reversed.size() != op.Inputs.size() ||
			 std::any_of(op.Reversed.begin(), op.Reversed.end(), [](uint8_t value) { return value > 1; })))
			return false;
		if (op.Kind == SourcePathOperationKind::VerletMesh) return ValidSourceVerletPath(op);
		if (op.Mesh || !op.CachedLengths.empty() || op.CachedTotalLength != 0) return false;
		if (op.Kind == SourcePathOperationKind::Shape)
			return op.Inputs.empty() && op.Shape && op.TrimRange == Vector2{0, 1} &&
				   ValidSourcePathShape(*op.Shape);
		if (op.Kind != SourcePathOperationKind::Reverse && op.Kind != SourcePathOperationKind::Combine &&
			op.Kind != SourcePathOperationKind::Trim && op.Kind != SourcePathOperationKind::Offset &&
			op.Kind != SourcePathOperationKind::Blend && op.Kind != SourcePathOperationKind::Join &&
			op.Kind != SourcePathOperationKind::Redistribute && op.Kind != SourcePathOperationKind::Skew)
			return false;
		if (op.Kind != SourcePathOperationKind::Combine && op.Kind != SourcePathOperationKind::Join &&
			op.Kind != SourcePathOperationKind::Blend && op.Inputs.size() > 1)
			return false;
		if (!std::isfinite(op.TrimRange.X) || !std::isfinite(op.TrimRange.Y)) return false;
		for (const auto &child : op.Inputs)
			if (!ValidSourcePath2D(child, depth + 1, count)) return false;
		return true;
	}
	template <bool Retained> inline uint64_t SourcePath2DBytes(const Path2D &path, size_t depth = 0) {
		if (depth > Limits::MaximumArrayDepth) return std::numeric_limits<uint64_t>::max();
		uint64_t bytes = (Retained ? path.Anchors.capacity() : path.Anchors.size()) * sizeof(PathAnchor) +
						 (Retained ? path.Weights.capacity() : path.Weights.size()) * sizeof(PathWeight);
		if (path.SourceOperation) {
			const auto &op = *path.SourceOperation;
			const uint64_t own = sizeof(op) +
								 (Retained ? op.Inputs.capacity() : op.Inputs.size()) * sizeof(Path2D) +
								 (Retained ? op.Reversed.capacity() : op.Reversed.size());
			if (own > std::numeric_limits<uint64_t>::max() - bytes)
				return std::numeric_limits<uint64_t>::max();
			bytes += own;
			const auto add = [&](uint64_t value) {
				if (value > std::numeric_limits<uint64_t>::max() - bytes) return false;
				bytes += value;
				return true;
			};
			if (!add((Retained ? op.BlendLengths.capacity() : op.BlendLengths.size()) * sizeof(double)) ||
				!add(
					(Retained ? op.BlendAccumulated.capacity() : op.BlendAccumulated.size()) *
					sizeof(std::vector<double>)
				))
				return std::numeric_limits<uint64_t>::max();
			for (const auto &row : op.BlendAccumulated)
				if (!add((Retained ? row.capacity() : row.size()) * sizeof(double)))
					return std::numeric_limits<uint64_t>::max();

			if (op.Shape) {
				const uint64_t points =
					(Retained ? op.Shape->Points.capacity() : op.Shape->Points.size()) * sizeof(Vector2);
				if (points > std::numeric_limits<uint64_t>::max() - bytes)
					return std::numeric_limits<uint64_t>::max();
				bytes += points;
			}
			const uint64_t meshBytes = SourceVerletPathBytes<Retained>(op);
			if (meshBytes > std::numeric_limits<uint64_t>::max() - bytes)
				return std::numeric_limits<uint64_t>::max();
			bytes += meshBytes;
			for (const auto &child : op.Inputs) {
				const uint64_t next = SourcePath2DBytes<Retained>(child, depth + 1);
				if (next > std::numeric_limits<uint64_t>::max() - bytes)
					return std::numeric_limits<uint64_t>::max();
				bytes += next;
			}
		}
		return bytes;
	}
}
