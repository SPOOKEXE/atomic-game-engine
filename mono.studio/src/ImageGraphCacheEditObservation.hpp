#pragma once

#include <engine/imagegraph/FeedbackHost.hpp>

namespace studio {
	class ImageGraphHistory;
	struct ImageGraphPlayback;
}

namespace studio::detail {
	// Source setters and animator undo wake caches. Timeline gestures only request a render.
	enum class ImageGraphCacheEditKind { ValueSetter, RenderOnly, AnimatorUndo, FreshDocument };
	// Prior input observation is derived comparison data, never an alternative authored document.
	struct ImageGraphCacheEditObservation {
		engine::imagegraph::Document Inputs;
		bool Ready = false;
		bool PendingValueEdit = false;
		ImageGraphCacheEditKind PendingValueKind = ImageGraphCacheEditKind::ValueSetter;
	};
	class ImageGraphCacheEditScope {
		ImageGraphCacheEditKind &Current;
		ImageGraphCacheEditKind Previous;

	  public:
		ImageGraphCacheEditScope(ImageGraphCacheEditKind &current, ImageGraphCacheEditKind next)
			: Current(current), Previous(current) {
			Current = next;
		}
		~ImageGraphCacheEditScope() {
			Current = Previous;
		}
	};
	// grug admit native history before publishing membership. history has its own serialized byte cap.
	[[nodiscard]] bool ApplyImageGraphCacheGroupMember(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		engine::imagegraph::CapturedFeedbackHost &host,
		std::string_view ownerId,
		std::string_view memberId,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
	// grug speculative history changes stage playback and runtime admission before document publication.
	// refusal preserves history, playback, inputs, pending edit kind and both host journals.
	[[nodiscard]] bool ApplyImageGraphCacheHistory(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		engine::imagegraph::CapturedFeedbackHost &host,
		ImageGraphCacheEditObservation &observation,
		ImageGraphPlayback &playback,
		bool redo,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
	// Admission or action failure preserves prior inputs and replay journals. A failed value edit
	// must be retried as a value/undo event before a render-only notification can consume it. fresh loads
	// stage membership and replace the host only after admission.
	[[nodiscard]] bool ObserveImageGraphCacheEdits(
		const engine::imagegraph::Document &document,
		ImageGraphCacheEditObservation &observation,
		engine::imagegraph::CapturedFeedbackHost &host,
		ImageGraphCacheEditKind kind,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
