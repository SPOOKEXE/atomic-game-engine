#pragma once

#include <engine/imagegraph/Document.hpp>

#include <string>
#include <vector>

namespace engine::imagegraph {
	enum class SourceBuiltinRandomOperation : uint8_t { Random, IRandom, IRandomRange, CRand };
	struct SourceBuiltinRandomDraw {
		SourceBuiltinRandomOperation Operation = SourceBuiltinRandomOperation::Random;
		double Lower = 0, Upper = 0, Result = 0;
		bool operator==(const SourceBuiltinRandomDraw &) const = default;
	};
	// Desktop builtin observations own the authored node, resolved controls and ordered draws.
	struct SourceBuiltinRandomCapture {
		Node Authored;
		size_t ProcessorRow = 0;
		uint64_t Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		std::vector<AuthoredValue> Inputs;
		std::vector<SourceBuiltinRandomDraw> Draws;
		bool operator==(const SourceBuiltinRandomCapture &) const = default;
	};
	// Preparation resolves a non-batched node's controls without executing its random calls.
	Status PrepareSourceBuiltinRandomCapture(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		SourceBuiltinRandomCapture &capture,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Validates recordings and returns the larger of copy cost and retained capacity per recording.
	// Borrowed evaluation spans and owned replay copies both admit that byte charge.
	Status ValidateBuiltinRandomCaptures(
		std::span<const SourceBuiltinRandomCapture> captures,
		uint64_t maximumBytes,
		uint64_t &ownedBytes,
		Diagnostic &diagnostic
	);
}
