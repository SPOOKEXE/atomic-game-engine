#pragma once

#include "NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	// grug replace mesh aliases with current constructor-owned state before evaluating consumers.
	bool ResolveSimulationInputAliases(NodeContext &context);
	Status ResolveSimulationValueAliases(
		Value &value,
		const SimulationReplayState &replay,
		EvaluationBudget &budget,
		AllocationReservation &outputCharge,
		Diagnostic &diagnostic
	);
	bool PublishSimulationMeshUpdate(NodeContext &context, const MeshValue2D &mesh);
	bool PublishSimulationFluidUpdate(NodeContext &context, const FluidDomainValue &domain);
	const SimulationReplayEntry *
	FindFluidSimulationOrigin(const NodeContext &context, std::string_view origin, size_t processorRow = 0);
	const SimulationReplayEntry *
	FindSimulationOrigin(const NodeContext &context, std::string_view origin, size_t processorRow = 0);
}
