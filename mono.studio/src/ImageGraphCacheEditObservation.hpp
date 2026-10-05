#pragma once

#include <engine/imagegraph/FeedbackHost.hpp>

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
	// Admission or action failure preserves prior inputs and replay journals. A failed value edit
	// must be retried as a value/undo event before a render-only notification can consume it. Fresh loads
	// replace the observation and reset the host without manufacturing interactive edits.
	[[nodiscard]] bool ObserveImageGraphCacheEdits(
		const engine::imagegraph::Document &document,
		ImageGraphCacheEditObservation &observation,
		engine::imagegraph::CapturedFeedbackHost &host,
		ImageGraphCacheEditKind kind,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
