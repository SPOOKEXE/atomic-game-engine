#pragma once

#include <engine/imagegraphio/SourceArtworkEdit.hpp>

#include <studio/ImageGraph.hpp>

namespace studio::detail {
	// History admission is the last fallible step. Documents and animator owners publish together.
	inline bool ApplyPreparedImageGraphArtwork(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		engine::imagegraph::GroupReplayState &replay,
		const engine::imagegraph::HostNodeCapture &capture,
		const engine::imagegraphio::SourceArtworkEditOptions &options,
		engine::imagegraph::Diagnostic &diagnostic,
		bool &changed,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		using namespace engine::imagegraph;
		changed = false;
		Document candidate;
		GroupReplayState next;
		if (engine::imagegraphio::ApplySourceArtworkEdit(
				document, capture, replay, options, candidate, next, diagnostic, maximumBytes
			) != Status::Ok)
			return false;
		if (candidate == document) return true;
		if (!history.TryRecord(document, candidate)) {
			diagnostic = {
				Status::LimitExceeded,
				capture.Authored.Id,
				{},
				"Artwork undo transaction exceeds history budget"
			};
			return false;
		}
		document = std::move(candidate);
		replay = std::move(next);
		changed = true;
		return true;
	}
}
