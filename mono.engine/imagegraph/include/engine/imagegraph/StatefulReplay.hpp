#pragma once

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/RandomReplay.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/imagegraph/SurfaceFrameReplay.hpp>

namespace engine::imagegraph {
	struct StatefulEvaluationResult {
		std::variant<Image, ImageArray, EvaluatedValue> Output;
		SimulationReplayState Simulation;
		SurfaceFrameReplayState Surfaces;
		RandomReplayState Random;
		DataReplayState Data;
	};
	struct StatefulNamedOutput {
		std::string Id;
		std::variant<Image, ImageArray, EvaluatedValue> Output;
	};
	struct StatefulInputEvaluationResult {
		EvaluationSnapshot Inputs;
		std::vector<StatefulNamedOutput> Outputs;
		SimulationReplayState Simulation;
		SurfaceFrameReplayState Surfaces;
		RandomReplayState Random;
		DataReplayState Data;
	};
	// grug capture resolved inputs and replay owners before the selected host kernel runs.
	Status EvaluateStatefulNodeInputs(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		StatefulInputEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes,
		std::span<const std::string> outputIds = {}
	);
	struct StatefulOutputEvaluationResult {
		std::vector<StatefulNamedOutput> Outputs;
		SimulationReplayState Simulation;
		SurfaceFrameReplayState Surfaces;
		RandomReplayState Random;
		DataReplayState Data;
	};
	uint64_t RetainedStatefulOutputBytes(const StatefulOutputEvaluationResult &result);
	// grug run the union of selected closures once, then publish every output and replay owner together.
	Status EvaluateStatefulOutputs(
		const Document &document,
		const Plan &plan,
		std::span<const std::string> outputIds,
		const EvaluationRequest &request,
		StatefulOutputEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Publishes selected closure output and both replay ledgers together. Existing surface cache entries
	// survive authored edits; callers explicitly reset by supplying an empty surface replay owner.
	Status EvaluateStateful(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		StatefulEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
