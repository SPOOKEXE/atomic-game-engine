#pragma once

#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/imagegraphio/SourceFrameCache.hpp>
#include <engine/imagegraphio/SourceImageEdit.hpp>

namespace engine::imagegraphexport {
	// A foreign surface layout is an explicit host observation tied to exact saved
	// cache text. Native caches carry the same identity in their engine annotation.
	using GraphImageCacheLayoutObservation = imagegraphio::SourceFrameCacheLayoutObservation;
	// Admit one bounded NODE:HASH=LAYOUT receipt; refusal preserves the existing ledger.
	bool AddGraphImageCacheLayoutObservation(
		std::string_view assignment,
		std::vector<GraphImageCacheLayoutObservation> &observations,
		std::string &failure
	);
	// Cook saved frame caches into one native .graph file through exact host paths.
	// A complete checked document is staged before publication; refusal preserves prior bytes.
	bool CookGraphSourceFrameCaches(
		const std::filesystem::path &input,
		const std::filesystem::path &output,
		std::span<const GraphImageCacheLayoutObservation> observations,
		const assets::ContentPolicy &policy,
		std::string &failure,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
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
