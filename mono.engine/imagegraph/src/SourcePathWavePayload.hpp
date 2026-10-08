#pragma once

#include "SourcePathSequentialPayload.hpp"
#include "SourcePathShiftKey.hpp"

#include <algorithm>
#include <tuple>
#include <vector>

namespace engine::imagegraph::detail {
	inline bool ValidSourceWave(const SourcePathWaveData2D &wave, size_t &count) {
		if (count > Limits::MaximumArrayElements || wave.Iteration < 0 || wave.Iteration > 4096 ||
			wave.Direction > 1 || wave.Mode > 2 || wave.Post > 2 || wave.WeightMode > 2 ||
			wave.AmplitudeCurve.size() != 129 ||
			(!wave.DirectionCurve.empty() && wave.DirectionCurve.size() != 33))
			return false;
		size_t extra = 0;
		for (size_t size : {wave.AmplitudeCurve.size(), wave.DirectionCurve.size(), wave.Cache.size()}) {
			if (extra > Limits::MaximumArrayElements - count ||
				size > Limits::MaximumArrayElements - count - extra)
				return false;
			extra += size;
		}
		count += extra;
		for (double value :
			 {wave.Range.X,
			  wave.Range.Y,
			  wave.Frequency.X,
			  wave.Frequency.Y,
			  wave.Amplitude.X,
			  wave.Amplitude.Y,
			  wave.Phase.X,
			  wave.Phase.Y,
			  wave.DirectionRange.X,
			  wave.DirectionRange.Y,
			  wave.WiggleAmplitude.X,
			  wave.WiggleAmplitude.Y,
			  wave.WeightRange.X,
			  wave.WeightRange.Y,
			  wave.Seed,
			  wave.AmplitudeShift,
			  wave.IterationFrequency,
			  wave.IterationAmplitude,
			  wave.IterationShift,
			  wave.WiggleFrequency})
			if (!std::isfinite(value)) return false;
		for (const auto *table : {&wave.AmplitudeCurve, &wave.DirectionCurve})
			for (double value : *table)
				if (!std::isfinite(value)) return false;
		for (const auto &buffer : wave.Buffers) {
			if ((buffer.Class != SourcePathPointClass::Planar &&
				 buffer.Class != SourcePathPointClass::Spatial) ||
				!std::isfinite(buffer.Position.X) || !std::isfinite(buffer.Position.Y) ||
				!std::isfinite(buffer.Weight) || (buffer.Z && !std::isfinite(*buffer.Z)) ||
				(buffer.Class == SourcePathPointClass::Spatial && !buffer.Z))
				return false;
		}
		struct CacheIdentity {
			SourcePathShiftKey Key;
			uint32_t Line;
		};
		if (wave.Cache.size() > Limits::MaximumEvaluationBytes / sizeof(CacheIdentity)) return false;
		std::vector<CacheIdentity> identities;
		identities.reserve(wave.Cache.size());
		for (const auto &point : wave.Cache) {
			const auto key = SourceShiftRatioKey(point.Coordinate);
			if (!key || point.Line >= Limits::MaximumArrayElements ||
				!std::isfinite(point.Point.Position.X) || !std::isfinite(point.Point.Position.Y) ||
				!std::isfinite(point.Point.Weight))
				return false;
			identities.push_back({*key, point.Line});
		}
		const auto lessIdentity = [](const CacheIdentity &left, const CacheIdentity &right) {
			return std::tie(left.Line, left.Key.Size, left.Key.Text) <
				   std::tie(right.Line, right.Key.Size, right.Key.Text);
		};
		std::sort(identities.begin(), identities.end(), lessIdentity);
		for (size_t index = 1; index < identities.size(); ++index)
			if (identities[index - 1].Line == identities[index].Line &&
				identities[index - 1].Key == identities[index].Key)
				return false;
		return true;
	}
	template <bool retained> uint64_t SourceWaveBytes(const SourcePathWaveData2D &wave) {
		return sizeof(wave) +
			   (retained ? wave.AmplitudeCurve.capacity() : wave.AmplitudeCurve.size()) * sizeof(double) +
			   (retained ? wave.DirectionCurve.capacity() : wave.DirectionCurve.size()) * sizeof(double) +
			   (retained ? wave.Cache.capacity() : wave.Cache.size()) *
				   sizeof(SourcePathSequentialCachePoint);
	}
} // namespace engine::imagegraph::detail
