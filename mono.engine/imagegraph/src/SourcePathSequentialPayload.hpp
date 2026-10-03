#pragma once
#include "MeshPayload.hpp"
#include "SourcePathShiftKey.hpp"

#include <engine/imagegraph/Document.hpp>

#include <cmath>
namespace engine::imagegraph::detail {
	inline bool SourceSequentialKind(SourcePathOperationKind kind) {
		return kind == SourcePathOperationKind::Extends || kind == SourcePathOperationKind::Flatten ||
			   kind == SourcePathOperationKind::Smoothen;
	}
	inline bool
	ValidSourceSequential(const SourcePathSequentialData2D &s, SourcePathOperationKind kind, size_t *count) {
		if (!SourceSequentialKind(kind) || s.ExtendSide > 1 ||
			s.SmoothSteps > int64_t(Limits::MaximumArrayElements) ||
			s.CachedSegments > Limits::MaximumArrayElements)
			return false;
		for (double n :
			 {s.ExtendLength,
			  s.SmoothRange.X,
			  s.SmoothRange.Y,
			  s.SmoothSpan,
			  s.SmoothBlend,
			  s.CachedLength,
			  s.CachedBounds.X,
			  s.CachedBounds.Y,
			  s.CachedBounds.Z,
			  s.CachedBounds.W,
			  s.ExtendStartPoint.Position.X,
			  s.ExtendStartPoint.Position.Y,
			  s.ExtendStartPoint.Weight,
			  s.ExtendEndPoint.Position.X,
			  s.ExtendEndPoint.Position.Y,
			  s.ExtendEndPoint.Weight,
			  s.ExtendStartDirection,
			  s.ExtendEndDirection,
			  s.SmoothPoint.Weight,
			  s.SmoothProbe.Weight})
			if (!std::isfinite(n)) return false;
		for (const auto &buffer : {s.SmoothPoint, s.SmoothProbe}) {
			if (buffer.Class != SourcePathPointClass::Planar && buffer.Class != SourcePathPointClass::Spatial)
				return false;
			if (!MeshFinite(buffer.Position) || !std::isfinite(buffer.Weight) ||
				(buffer.Z && !std::isfinite(*buffer.Z)) ||
				(buffer.Class == SourcePathPointClass::Spatial && !buffer.Z))
				return false;
		}
		const auto add = [&](size_t n) {
			if (n > Limits::MaximumArrayElements - *count) return false;
			*count += n;
			return true;
		};
		if (!add(1) || !add(s.Accumulated.size()) || !add(s.FlattenLengths.size()) ||
			!add(s.FlattenOwners.size()) || !add(s.Cache.size()))
			return false;
		for (double n : s.Accumulated)
			if (!std::isfinite(n)) return false;
		for (double n : s.FlattenLengths)
			if (!std::isfinite(n)) return false;
		if (s.FlattenLengths.size() != s.FlattenOwners.size()) return false;
		for (const auto &sample : s.Cache)
			if (!std::isfinite(sample.Coordinate) || !MeshFinite(sample.Point.Position) ||
				!std::isfinite(sample.Point.Weight) || sample.Line > Limits::MaximumArrayElements ||
				(kind != SourcePathOperationKind::Smoothen && sample.Line))
				return false;
		for (size_t i = 0; i < s.Cache.size(); ++i) {
			const auto key = kind == SourcePathOperationKind::Smoothen
								 ? SourceShiftRatioKey(s.Cache[i].Coordinate)
								 : SourcePathDistanceKey(s.Cache[i].Coordinate);
			if (!key) return false;
			for (size_t j = 0; j < i; ++j) {
				if (s.Cache[j].Line != s.Cache[i].Line) continue;
				const auto previous = kind == SourcePathOperationKind::Smoothen
										  ? SourceShiftRatioKey(s.Cache[j].Coordinate)
										  : SourcePathDistanceKey(s.Cache[j].Coordinate);
				if (previous && *previous == *key) return false;
			}
		}
		if (kind != SourcePathOperationKind::Flatten &&
			(!s.FlattenLengths.empty() || !s.FlattenOwners.empty() || s.FlattenReverse || s.FlattenPingPong))
			return false;
		if (kind != SourcePathOperationKind::Extends &&
			(s.ExtendSide || s.ExtendLength != 16 || s.ExtendStartPoint != SourcePathEndpoint2D{} ||
			 s.ExtendEndPoint != SourcePathEndpoint2D{} || s.ExtendStartDirection != 0 ||
			 s.ExtendEndDirection != 0))
			return false;
		if (kind != SourcePathOperationKind::Smoothen &&
			(s.SmoothRange != Vector2{0, 1} || s.SmoothClampCurve || s.SmoothLoop || s.SmoothSpan != .02 ||
			 s.SmoothBlend != 1 || s.SmoothSteps != 1 || s.SmoothPoint != SourcePathPointBuffer{} ||
			 s.SmoothProbe != SourcePathPointBuffer{}))
			return false;
		if (kind == SourcePathOperationKind::Smoothen &&
			(!s.Accumulated.empty() || s.CachedSegments != 0 || s.CachedLength != 0 ||
			 s.CachedBounds != Vector4{0, 0, 1, 1}))
			return false;
		return true;
	}
	template <bool Retained> inline uint64_t SourceSequentialBytes(const SourcePathSequentialData2D &s) {
		uint64_t bytes = sizeof(s);
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(s.Accumulated));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(s.FlattenLengths));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(s.FlattenOwners));
		return MeshAddBytes(bytes, MeshVectorBytes<Retained>(s.Cache));
	}
}
