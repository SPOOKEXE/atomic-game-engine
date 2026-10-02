#pragma once
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
namespace engine::imagegraph {
	std::string_view BuiltinRandomOperationName(SourceBuiltinRandomOperation operation) noexcept;
	std::optional<SourceBuiltinRandomOperation>
	ParseBuiltinRandomOperationName(std::string_view name) noexcept;
	// Encodes copied desktop observations, never a replacement random generator. Failure preserves text.
	Status WriteBuiltinRandomCapture(
		std::span<const SourceBuiltinRandomCapture> captures,
		std::string &text,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Admits source text, prior destination and candidate residency before allocation. Failure preserves
	// captures.
	Status ReadBuiltinRandomCapture(
		std::string_view text,
		std::vector<SourceBuiltinRandomCapture> &captures,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
