#pragma once
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>

namespace engine::imagegraphexport {
	// Literal operation:lower:upper:result observation. No generator or shell
	// interpretation.
	bool ParseBuiltinRandomDraw(
		std::string_view text, engine::imagegraph::SourceBuiltinRandomDraw &draw, std::string &failure
	);
	// Reads one exact caller pathname with the configured content policy and
	// bounded copied observations.
	bool LoadBuiltinRandomCaptureFile(
		const std::filesystem::path &file,
		const engine::assets::ContentPolicy &policy,
		std::vector<engine::imagegraph::SourceBuiltinRandomCapture> &captures,
		std::string &failure,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
	// Resolves one non-batched authored node without executing its random calls,
	// attaches supplied draws, then atomically publishes the complete capture to an
	// exact caller-selected destination.
	bool PrepareBuiltinRandomCaptureFile(
		const GraphExportSettings &settings,
		std::string_view nodeId,
		const std::filesystem::path &destination,
		std::span<const engine::imagegraph::SourceBuiltinRandomDraw> draws,
		std::string &failure,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
} // namespace engine::imagegraphexport
