#pragma once
#include "MeshPayload.hpp"
#include "SourcePathPayload.hpp"
namespace engine::imagegraph::detail {
	inline bool ValidSourcePath3D(const PathData3D &path, size_t depth = 0, size_t *count = nullptr) {
		size_t local = 0;
		if (!count) count = &local;
		if (depth > Limits::MaximumArrayDepth || *count >= Limits::MaximumArrayElements) return false;
		++*count;
		if (path.Anchors.size() > Limits::MaximumPathAnchors || !path.Resolution ||
			path.Resolution > Limits::MaximumArrayElements ||
			path.Transforms.size() > Limits::MaximumArrayDepth)
			return false;
		if (path.SourcePolyline &&
			(!path.SourcePresent || path.Source2D || path.SourceOperation || path.Resolution != 1))
			return false;
		if (path.SourceBounds2D &&
			(!path.SourcePolyline ||
			 (!std::isfinite(path.SourceBounds2D->X) || !std::isfinite(path.SourceBounds2D->Y) ||
			  !std::isfinite(path.SourceBounds2D->Z) || !std::isfinite(path.SourceBounds2D->W))))
			return false;
		if (path.SourceEmptyCache &&
			(!path.SourcePolyline || !path.Anchors.empty() || !std::isfinite(path.SourceEmptyCache->Length) ||
			 path.SourceEmptyCache->Length < 0 || !path.SourceEmptyCache->SegmentCount ||
			 path.SourceEmptyCache->SegmentCount > Limits::MaximumPathAnchors))
			return false;
		if (path.SourcePolyline && path.Loop && !path.Anchors.empty()) {
			if (path.Anchors.size() < 2) return false;
			for (size_t i = 0; i < 3; ++i)
				if (path.Anchors.front().Controls[i] != path.Anchors.back().Controls[i]) return false;
		}
		if (path.SourceEmptyCache) {
			const auto &cache = *path.SourceEmptyCache;
			if ((!cache.Accumulated.empty() && cache.Accumulated.size() != cache.SegmentCount) ||
				cache.Accumulated.size() > Limits::MaximumArrayElements - *count)
				return false;
			*count += cache.Accumulated.size();
			double previous = 0;
			for (double length : cache.Accumulated) {
				if (!std::isfinite(length) || length < previous) return false;
				previous = length;
			}
			if (!cache.Accumulated.empty() && cache.Accumulated.back() != cache.Length) return false;
		}
		for (const auto &a : path.Anchors) {
			if (!std::isfinite(a.Index)) return false;
			for (double n : a.Controls)
				if (!std::isfinite(n)) return false;
			if (path.SourcePolyline)
				for (size_t i = 3; i < 9; ++i)
					if (a.Controls[i] != 0) return false;
		}
		for (const auto &t : path.Transforms) {
			if (!MeshFinite(t.Position) || !MeshFinite(t.Anchor) || !MeshFinite(t.Scale) ||
				!MeshFinite(t.Rotation) || !MeshFinite(t.ProjectionScale))
				return false;
			for (double n : t.CameraView)
				if (!std::isfinite(n)) return false;
			for (double n : t.CameraProjection)
				if (!std::isfinite(n)) return false;
		}
		if (path.Source2D && !ValidSourcePath2D(*path.Source2D, depth + 1, count)) return false;
		if (!path.SourceOperation) return true;
		if (!path.SourcePresent || path.Source2D || !path.Anchors.empty() || path.Loop) return false;
		const auto &op = *path.SourceOperation;
		if (op.Kind != SourcePathOperationKind::Reverse && op.Kind != SourcePathOperationKind::Combine &&
			op.Kind != SourcePathOperationKind::Trim)
			return false;
		if (op.Kind != SourcePathOperationKind::Combine && op.Inputs.size() > 1) return false;
		if (!MeshFinite(op.TrimRange)) return false;
		for (const auto &child : op.Inputs)
			if (!child.Data || !ValidSourcePath3D(*child.Data, depth + 1, count)) return false;
		return true;
	}
	template <bool Retained> inline uint64_t SourcePath3DBytes(const PathData3D &path, size_t depth = 0) {
		if (depth > Limits::MaximumArrayDepth) return UINT64_MAX;
		uint64_t bytes = MeshAddBytes(sizeof(path), MeshVectorBytes<Retained>(path.Anchors));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(path.Transforms));
		if (path.SourceEmptyCache)
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(path.SourceEmptyCache->Accumulated));
		if (path.Source2D)
			bytes = MeshAddBytes(bytes, SourcePath2DBytes<Retained>(*path.Source2D, depth + 1));
		if (path.SourceOperation) {
			const auto &op = *path.SourceOperation;
			bytes = MeshAddBytes(bytes, MeshAddBytes(sizeof(op), MeshVectorBytes<Retained>(op.Inputs)));
			for (const auto &child : op.Inputs)
				if (child.Data)
					bytes = MeshAddBytes(bytes, SourcePath3DBytes<Retained>(*child.Data, depth + 1));
		}
		return bytes;
	}
}
