#pragma once

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/SourceCommonAnimatorReset.hpp>

namespace engine::imagegraph::detail {
	// The caller already charges the full prior and candidate sessions. Only selected replacement
	// overlap is admitted here; publication transfers its charge and releases the old selected payload.
	Status ResetSourceCommonAnimatorBudgeted(
		const DetachedSourceAnimator &animator,
		GroupSubtypeOverlay &candidatePayload,
		const SourceCommonAnimatorResetOptions &options,
		SourceCommonAnimatorResetReceipt &receipt,
		Diagnostic &diagnostic,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge
	);
}
