#pragma once
#include "SourcePathSequentialPayload.hpp"
#include "SourcePathShiftKey.hpp"
namespace engine::imagegraph::detail {
	inline bool ValidSourceSpiral(const SourcePathSpiralData2D &s, size_t &count) {
		if (s.Direction > 1 || s.WeightMode > 2 ||
			(!s.AmplitudeCurve.empty() && s.AmplitudeCurve.size() != 129) ||
			(!s.DirectionCurve.empty() && s.DirectionCurve.size() != 33))
			return false;
		const size_t extra = s.AmplitudeCurve.size() + s.DirectionCurve.size() + s.Cache.size();
		if (extra > Limits::MaximumArrayElements - count) return false;
		count += extra;
		for (double n :
			 {s.Range.X,
			  s.Range.Y,
			  s.DirectionRange.X,
			  s.DirectionRange.Y,
			  s.WeightRange.X,
			  s.WeightRange.Y,
			  s.Frequency,
			  s.Amplitude,
			  s.Spiral,
			  s.Phase})
			if (!std::isfinite(n)) return false;
		for (const auto *table : {&s.AmplitudeCurve, &s.DirectionCurve})
			for (double n : *table)
				if (!std::isfinite(n)) return false;
		for (const auto &p : s.Buffers)
			if (p.Class != SourcePathPointClass::Planar || p.Z ||
				(!std::isfinite(p.Position.X) || !std::isfinite(p.Position.Y) || !std::isfinite(p.Weight)))
				return false;
		for (size_t i = 0; i < s.Cache.size(); ++i) {
			const auto &p = s.Cache[i];
			const auto key = SourceShiftRatioKey(p.Coordinate);
			if (!key || p.Line >= Limits::MaximumArrayElements || !std::isfinite(p.Point.Position.X) ||
				!std::isfinite(p.Point.Position.Y) || !std::isfinite(p.Point.Weight))
				return false;
			for (size_t j = 0; j < i; ++j)
				if (s.Cache[j].Line == p.Line && SourceShiftRatioKey(s.Cache[j].Coordinate) == key)
					return false;
		}
		return true;
	}
	template <bool retained> uint64_t SourceSpiralBytes(const SourcePathSpiralData2D &s) {
		return sizeof(s) +
			   (retained ? s.AmplitudeCurve.capacity() : s.AmplitudeCurve.size()) * sizeof(double) +
			   (retained ? s.DirectionCurve.capacity() : s.DirectionCurve.size()) * sizeof(double) +
			   (retained ? s.Cache.capacity() : s.Cache.size()) * sizeof(SourcePathSequentialCachePoint);
	}
}
