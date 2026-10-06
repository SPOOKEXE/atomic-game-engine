#pragma once
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
namespace engine::imagegraph {
	std::string_view BuiltinRandomOperationName(SourceBuiltinRandomOperation operation) noexcept;
	std::optional<SourceBuiltinRandomOperation>
	ParseBuiltinRandomOperationName(std::string_view name) noexcept;
	// writes v3 with retained axes, or v4 when axes stay stored but inactive. copies source origins and
	// observations. Failure preserves text.
	Status WriteBuiltinRandomCapture(
		std::span<const SourceBuiltinRandomCapture> captures,
		std::string &text,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// reads versions 1 through 4. admits source text, prior destination and candidate residency before
	// allocation. Failure preserves captures.
	Status ReadBuiltinRandomCapture(
		std::string_view text,
		std::vector<SourceBuiltinRandomCapture> &captures,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
