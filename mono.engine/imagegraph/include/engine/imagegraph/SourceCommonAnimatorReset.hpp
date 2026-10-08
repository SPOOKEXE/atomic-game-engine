#pragma once

#include <engine/imagegraph/FrameTime.hpp>

namespace engine::imagegraph {
	struct SourceCommonAnimatorResetOptions {
		FrameTime Time{};
		bool ReplaceExistingKey = true, UpdateOnSet = false;
	};
	struct SourceCommonAnimatorResetReceipt {
		bool Modified = true, ForceDynamic = true, Edited = false, AnimatorReturnedTrue = false;
		bool operator==(const SourceCommonAnimatorResetReceipt &) const = default;
	};
	// The caller resolves the validated local physical Update animator, separately from its getter.
	// Only the original Trigger setter is supported. This edits its sole payload without callbacks,
	// undo history, upstream writes or readiness changes. Failure preserves payload and receipt.
	Status ResetSourceCommonAnimator(
		const DetachedSourceAnimator &animator,
		const GroupSubtypeOverlay &payload,
		const SourceCommonAnimatorResetOptions &options,
		GroupSubtypeOverlay &result,
		SourceCommonAnimatorResetReceipt &receipt,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
