#pragma once
#include <engine/imagegraph/FrameTime.hpp>
namespace engine::imagegraph {
	// Source setFrame rounds the authoritative project clock to even before isLastFrame compares it to the
	// resolved endpoint.
	inline bool SourceFrameCacheIsLastProjectFrame(const SourceFrameCacheProjectObservation &observation) {
		if (!ValidFrameTime(observation.ProjectFrame) || !std::isfinite(observation.ProjectLastFrame))
			return false;
		FrameTime current, endpoint;
		return SplitFrameTime(double(FrameTimeToReal(observation.ProjectFrame)), current, true) &&
			   SplitFrameTime(observation.ProjectLastFrame, endpoint) && current == endpoint;
	}
	// Only this named native profile synthesizes played project observations for seek warmup.
	// ObservedFrame keeps the caller's authoritative project facts unchanged.
	inline void BindNativeSourceFrameCacheProjectPrefix(EvaluationRequest &request) {
		if (request.SourceCacheProject && request.SourceCachePlayback &&
			request.SourceCachePlayback->Sampling == SourceCacheSampling::NativePlayedPrefix)
			request.SourceCacheProject->ProjectFrame = {
				request.Tick, request.Subframe, request.NegativeFrame
			};
	}
}
