#pragma once
#include "SourceVerletPath.hpp"

#include <engine/imagegraph/Document.hpp>

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
		if (op.Kind == SourcePathOperationKind::VerletMesh) return ValidSourceVerletPath(op);
		if (op.Mesh || !op.CachedLengths.empty() || op.CachedTotalLength != 0) return false;
		if (op.Kind != SourcePathOperationKind::Reverse && op.Kind != SourcePathOperationKind::Combine &&
			op.Kind != SourcePathOperationKind::Trim)
			return false;
		if (op.Kind != SourcePathOperationKind::Combine && op.Inputs.size() > 1) return false;
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
			const uint64_t own =
				sizeof(op) + (Retained ? op.Inputs.capacity() : op.Inputs.size()) * sizeof(Path2D);
			if (own > std::numeric_limits<uint64_t>::max() - bytes)
				return std::numeric_limits<uint64_t>::max();
			bytes += own;
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
