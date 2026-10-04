#pragma once
#include <engine/imagegraph/FrameTime.hpp>
namespace engine::imagegraph {
	// Source isLastFrame compares the authoritative project's real current frame
	// to its resolved endpoint, independently of a node's scoped input clock.
	inline bool SourceFrameCacheIsLastProjectFrame(const SourceFrameCacheProjectObservation &observation) {
		return double(FrameTimeToReal(observation.ProjectFrame)) == observation.ProjectLastFrame;
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
