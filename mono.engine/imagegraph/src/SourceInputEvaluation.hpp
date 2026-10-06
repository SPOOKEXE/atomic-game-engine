#pragma once

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	// compile and sample one immutable document under the same live byte ledger.
	// caller keeps the document and prior result charged; the compiled plan stays local to this call.
	Status EvaluateSourceInput(
		const Document &document,
		std::string_view nodeId,
		std::string_view port,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		Value &result,
		AllocationReservation &resultCharge,
		Diagnostic &diagnostic,
		std::optional<std::span<const AuthoredValue>> observedInputs = std::nullopt,
		std::string_view observedInputOwner = {}
	);

	// one source Vec2 getter, with units disabled; siblings and the target processor stay asleep.
	// caller keeps document, plan and prior result charged in this same ledger.
	// self/node_values expressions need the host's retained map, including an explicitly empty map.
	// an omitted map owner means nodeId; inherited expressions may require their base owner instead.
	Status EvaluateSourceInput(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		std::string_view port,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		Value &result,
		AllocationReservation &resultCharge,
		Diagnostic &diagnostic,
		std::optional<std::span<const AuthoredValue>> observedInputs = std::nullopt,
		std::string_view observedInputOwner = {}
	);
}
