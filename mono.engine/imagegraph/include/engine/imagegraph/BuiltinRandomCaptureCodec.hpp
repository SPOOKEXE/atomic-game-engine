#pragma once
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
namespace engine::imagegraph {
	std::string_view BuiltinRandomOperationName(SourceBuiltinRandomOperation operation) noexcept;
	std::optional<SourceBuiltinRandomOperation>
	ParseBuiltinRandomOperationName(std::string_view name) noexcept;
	// writes v3 with axes, v4 with inactive axes, or v5 with local constructor defaults. copies origins and
	// observations. Failure preserves text.
	Status WriteBuiltinRandomCapture(
		std::span<const SourceBuiltinRandomCapture> captures,
		std::string &text,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// reads versions 1 through 5. admits source text, prior destination and candidate residency before
	// allocation. Failure preserves captures.
	Status ReadBuiltinRandomCapture(
		std::string_view text,
		std::vector<SourceBuiltinRandomCapture> &captures,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
