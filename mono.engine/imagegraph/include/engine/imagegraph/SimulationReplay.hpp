#pragma once

// grug state belongs to caller, keyed by durable node text. failed evaluation publishes no updates.

#include <engine/imagegraph/VerletReplay.hpp>

#include <string>
#include <vector>

namespace engine::imagegraph {
	struct VerletDragReplay {
		Vector2 Move{}, Previous{};
		std::optional<Vector2> Anchor;
		double PreviousAngle = 0;
		bool operator==(const VerletDragReplay &) const = default;
	};
	struct SimulationReplayEntry {
		std::string NodeId;
		VerletReplayState State;
		MeshTopology2D Topology;
		size_t ProcessorRow = 0;
		std::optional<VerletDragReplay> Drag;
		std::optional<std::vector<Vector2>> Cache;
		FluidDomainValue Fluid;
		bool operator==(const SimulationReplayEntry &) const = default;
	};
	struct SimulationReplayState {
		std::vector<SimulationReplayEntry> Entries;
		bool operator==(const SimulationReplayState &) const = default;
	};
	// grug retained bytes include capacity and node identity; overflow saturates the count.
	uint64_t RetainedSimulationReplayBytes(const SimulationReplayState &state);
	uint64_t RetainedSimulationEntryBytes(const SimulationReplayEntry &entry);
	// grug reject malformed or oversized borrowed ledgers before copying any entry.
	[[nodiscard]] Status ValidateSimulationReplay(
		const SimulationReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	);
	struct SimulationEvaluationResult {
		std::variant<Image, EvaluatedValue> Output;
		SimulationReplayState Replay;
	};
	// Evaluates one complete output closure and publishes its pixels/value and state updates together.
	// Unvisited authored nodes retain their previous state; removed nodes retire their entries.
	// Failure preserves the prior result and replay owner.
	Status EvaluateSimulation(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		SimulationEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);

}
