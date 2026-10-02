#pragma once
#include <engine/imagegraph/WavExport.hpp>

namespace studio {
	// Explicit host action. Failure preserves the destination and last successful artifact.
	bool ExportImageGraphWav(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		engine::imagegraph::WavExport &lastGood,
		engine::imagegraph::Diagnostic &diagnostic
	);
	// Inspector action shared with the headless ImGui functional suite.
	void DrawImageGraphWavExport(
		const engine::imagegraph::Document &document,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		engine::imagegraph::WavExport &lastGood,
		std::string &message
	);
}
