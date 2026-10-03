#pragma once
#include <engine/imagegraph/FeedbackHost.hpp>

#include <studio/ImageGraph.hpp>

namespace studio::detail {
	// Source Clear is runtime state, not a document edit or playback restart.
	inline bool ApplyImageGraphCacheResultsClear(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		engine::imagegraph::CapturedFeedbackHost &host,
		ImageGraphPreviewCache &previews,
		std::string_view nodeId,
		std::string_view selectedNode,
		uint64_t revision,
		uint64_t inputRevision,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		if (nodeId != selectedNode) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				std::string(nodeId),
				{},
				"Cache Results selection changed before Clear"
			};
			return false;
		}
		if (!host.ClearCacheResults(
				document, plan, nodeId, revision, inputRevision, diagnostic, maximumBytes
			))
			return false;
		for (size_t index = 0; index < document.Outputs.size(); ++index)
			if (std::find(
					host.CacheResultsInvalidatedOutputs().begin(),
					host.CacheResultsInvalidatedOutputs().end(),
					document.Outputs[index].Id
				) != host.CacheResultsInvalidatedOutputs().end())
				previews.InvalidateOutput(index);
		return true;
	}
}
