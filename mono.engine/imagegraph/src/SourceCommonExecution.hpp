#pragma once
#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/imagegraph/SourceCommonAnimatorReset.hpp>
#include <engine/imagegraph/SourceCommonDispatch.hpp>

namespace engine::imagegraph::detail {
	struct SourceCommonAdmission;
	enum class SourceCommonInvocationMode : uint8_t { DirectUpdate, SourceDoUpdate };
	Status ReadSourceCommonGetter(
		const Document &,
		const Plan &,
		std::string_view ownerId,
		SourceCommonSelector,
		const EvaluationRequest &,
		EvaluationBudget &,
		Value &,
		AllocationReservation &,
		Diagnostic &
	);
	Status InvokeSourceCommonCallback(
		const Document &,
		const Plan &,
		const EvaluationRequest &,
		size_t ownerIndex,
		SourceCommonInvocationMode,
		GroupRenderSession &,
		EvaluationBudget &,
		AllocationReservation &candidateCharge,
		Diagnostic &,
		SourceCommonAdmission * = nullptr
	);
}
namespace engine::imagegraph::detail {
	enum class SourceCommonPreparePurpose { Step, Reconcile };
	struct SourceCommonAdmission {
		AllocationReservation Document, Plan, Borrowed, FontHost, Observer;
	};
	Status PrepareSourceCommonCandidate(
		const Document &,
		const Plan &,
		const EvaluationRequest &,
		std::optional<SourceNodeInitialState>,
		const GroupRenderSession &prior,
		GroupRenderSession &candidate,
		EvaluationBudget &,
		AllocationReservation &candidateCharge,
		SourceCommonAdmission &,
		Diagnostic &,
		SourceCommonPreparePurpose = SourceCommonPreparePurpose::Step
	);
	SourceCommonDispatch
	SourceCommonOwnerDispatch(const Document &, const EvaluationRequest &, size_t ownerIndex);
	Status RefreshSourceCommonCollection(
		const Document &,
		const Plan &,
		const EvaluationRequest &,
		size_t ownerIndex,
		const SourcePurityRefresh &,
		GroupRenderSession &,
		EvaluationBudget &,
		AllocationReservation &candidateCharge,
		Diagnostic &
	);
	Status RecordSourceCommonAnimatorReset(
		GroupRenderSession &,
		const SourceCommonOwnerRecord &,
		const SourceCommonAnimatorResetReceipt &,
		EvaluationBudget &,
		AllocationReservation &candidateCharge,
		Diagnostic &
	);
}

namespace engine::imagegraph::detail {
	SourceSocketDomain SourceCommonGetterDomain(SourceCommonSelector);
	Status ValidateSourceCommonObservation(
		const Document &,
		const Plan &,
		const EvaluationRequest &,
		const GroupRenderSession &,
		EvaluationBudget &,
		SourceCommonAdmission &,
		Diagnostic &
	);
	Status ObserveSourceCommonCandidateOutputs(
		const Document &,
		const Plan &,
		const EvaluationRequest &,
		GroupRenderSession &,
		EvaluationBudget &,
		AllocationReservation &,
		Diagnostic &
	);
}

namespace engine::imagegraph::detail {}

namespace engine::imagegraph::detail {
	Status ReadHeldSourceCommonInput(
		const Document &,
		const Plan &,
		std::string_view nodeId,
		std::string_view port,
		const EvaluationRequest &,
		EvaluationBudget &,
		Value &,
		AllocationReservation &,
		Diagnostic &
	);
}
