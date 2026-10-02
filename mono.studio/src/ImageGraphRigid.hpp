#pragma once

#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	inline uint8_t ImageGraphRigidObservation(
		const engine::imagegraph::Document &document, const ImageGraphPlayback &playback
	) {
		if (std::none_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
				return node.Type.starts_with("pc.rigid_");
			}))
			return 0;
		return uint8_t((playback.Playing ? 1 : 0) | (playback.FrameProgress ? 2 : 0));
	}

	// The caller keeps the synchronous provider alive through the entire evaluation.
	inline void BindImageGraphRigid(
		engine::imagegraph::EvaluationRequest &request,
		engine::imagegraphphysics::RigidProvider &provider,
		const ImageGraphPlayback &playback
	) {
		request.RigidProvider = &provider;
		request.RigidPlaying = playback.Playing;
		request.RigidFrameProgress = playback.FrameProgress;
	}
}
