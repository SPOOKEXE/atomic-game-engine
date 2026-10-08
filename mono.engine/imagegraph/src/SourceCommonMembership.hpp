#pragma once

// Structural edits have explicit constructor intent. Ordinary evaluation cannot infer that intent.
#include "EvaluationBudget.hpp"

#include <engine/imagegraph/SourceCommonRuntime.hpp>

namespace engine::imagegraph::detail {
	// Selected process history only. The caller accounts for other session ledgers independently.
	uint64_t RetainedSourceCommonMembershipBytes(const GroupRenderSession &session);
	// The whole prior and candidate sessions are already charged. Replacement overlap shares that
	// same ledger. Failure preserves candidate and its charge; success transfers only selected rows.
	Status ReconcileSourceCommonMembership(
		const Document &document,
		SourceNodeInitialState newOwnerState,
		GroupRenderSession &candidate,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		Diagnostic &diagnostic,
		std::span<const SourceCommonWriterIdentity> resetWriters = {}
	);
}
