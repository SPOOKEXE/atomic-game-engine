#pragma once

#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/imagegraphio/SourceImageEdit.hpp>

namespace engine::imagegraphexport {
	// A foreign surface layout is an explicit host observation tied to exact saved
	// cache text. Native caches carry the same identity in their engine annotation.
	struct GraphImageCacheLayoutObservation {
		std::string NodeId;
		std::string DataHash;
		bake::SpriteCacheLayout Layout = bake::SpriteCacheLayout::Rgba8TopDown;
	};
	// Captures unpadded live source sprites even when an older cache is enabled.
	// encodeCache adds a native RGBA8 cache of this exact immutable ledger. No
	// borrowed invocation, grant, request or provider is retained in the result.
	bool PrepareGraphSourceImages(
		const imagegraph::HostNodeCapture &controls,
		std::span<const GraphFileGrant> grants,
		const assets::ContentPolicy &policy,
		uint64_t authoringRevision,
		uint64_t inputRevision,
		bool encodeCache,
		imagegraphio::SourceImageFrameObservation &result,
		std::string &failure,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
}
