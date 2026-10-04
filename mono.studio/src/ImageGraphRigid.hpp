#pragma once

#include "ImageGraphRegionBounds.hpp"

#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
#include <limits>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	inline uint8_t ImageGraphPlaybackObservation(
		const engine::imagegraph::Document &document, const ImageGraphPlayback &playback
	) {
		const bool rigid = std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
			return node.Type.starts_with("pc.rigid_");
		});
		const bool caches = std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
			return node.Type == "pc.cache" || node.Type == "pc.cache_array";
		});
		return uint8_t(
			(rigid ? ((playback.Playing ? 1 : 0) | (playback.FrameProgress ? 2 : 0)) : 0) |
			(caches && playback.Playing ? 4 : 0)
		);
	}

	// The caller keeps the synchronous provider alive through the entire evaluation.
	inline void BindImageGraphRigid(
		engine::imagegraph::EvaluationRequest &request,
		engine::imagegraphphysics::RigidProvider &provider,
		const ImageGraphPlayback &playback,
		bool projectLoading = false,
		bool projectAppending = false
	) {
		request.RigidProvider = &provider;
		request.RigidPlaying = playback.Playing;
		request.SourceCachePlayback = engine::imagegraph::SourceCachePlaybackObservation{
			playback.Playing, engine::imagegraph::SourceCacheSampling::ObservedFrame, true
		};
		request.RigidFrameProgress = playback.FrameProgress;
		request.SourceCacheProject = engine::imagegraph::SourceFrameCacheProjectObservation{
			GetImageGraphFrame(playback),
			SelectedRegionLastFrame(playback).value_or(std::numeric_limits<double>::quiet_NaN()),
			projectLoading,
			projectAppending
		};
	}
}
