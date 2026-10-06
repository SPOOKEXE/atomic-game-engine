#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::imagegraph {
	// grug borrow storage under its logical channel name; view owns no authored data.
	struct SourceAxisObservation {
		// logical names borrowed from the document, rather than physical key names.
		std::string_view NodeId, Port;
		// effective warm storage, including a retained writer after its property was replaced.
		const SourceSeparatedVec2Animator *Storage = nullptr;
		// true when logical names differ from storage names and capture clears physical key ids.
		bool Alias = false;
	};
	// grug borrow effective warm arrays once per logical input, including captured aliases.
	// cold arrays stay cold. document mutation or destruction invalidates names and storage.
	// failure preserves result; resident bytes, lookup work and logical key fanout are bounded.
	Status ObserveSourceKeyframeAxes(
		const Document &document,
		std::vector<SourceAxisObservation> &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

	struct SourceKeyframeIdentity {
		std::string_view NodeId, Port;
		FrameTime Time;
		int8_t Axis = -1;
	};
	// Captures exact logical pins in selection order. Scalar pins read the independently captured
	// writer even without local alias projections. A bounded Compile validates the document first.
	// Failure preserves result; clocks are not clamped.
	Status CaptureSourceKeyframes(
		const Document &document,
		std::span<const SourceKeyframeIdentity> selection,
		std::vector<Keyframe> &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

	struct SourceKeyframeEdit {
		const Keyframe *Original = nullptr;
		// Null removes the pinned key. Replacement keeps the original logical socket.
		const Keyframe *Replacement = nullptr;
		bool Copy = false;
		// -1 selects the combined animator; 0 and 1 select its independently captured X/Y arrays.
		int8_t Axis = -1;
	};
	// Edits combined and scalar-axis writers and their compatibility projections in one transaction.
	// Moves keep physical key IDs; copies reset IDs and drivers. Duplicate alias selections must
	// agree. All originals are removed before insertion; the first moved collision wins.
	// Clocks are used verbatim. Warm axes can be edited while combined; cold arrays require initialization.
	// Failure preserves result. A bounded Compile validates the candidate before publication.
	Status ApplySourceKeyframeEdits(
		const Document &document,
		std::span<const SourceKeyframeEdit> edits,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
