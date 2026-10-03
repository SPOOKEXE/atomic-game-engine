#pragma once
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
namespace engine::imagegraph {
	std::string_view BuiltinRandomOperationName(SourceBuiltinRandomOperation operation) noexcept;
	std::optional<SourceBuiltinRandomOperation>
	ParseBuiltinRandomOperationName(std::string_view name) noexcept;
	// Writes version 2 with copied source-input origins and desktop observations, never generated draws.
	// Failure preserves text.
	Status WriteBuiltinRandomCapture(
		std::span<const SourceBuiltinRandomCapture> captures,
		std::string &text,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Reads versions 1 and 2. Admits source text, prior destination and candidate residency before
	// allocation. Failure preserves captures.
	Status ReadBuiltinRandomCapture(
		std::string_view text,
		std::vector<SourceBuiltinRandomCapture> &captures,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
