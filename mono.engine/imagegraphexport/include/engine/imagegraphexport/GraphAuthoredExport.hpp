#pragma once
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>

namespace engine::imagegraphexport {
	struct GraphExportRegion {
		std::string Name;
		engine::imagegraph::TickRange Frames;
	};
	// Explicit execution grants a directory. Every expanded authored name must remain inside it.
	bool PlanAuthoredGraphExport(
		const engine::imagegraph::HostNodeInvocation &invocation,
		const GraphExportSettings &grants,
		std::span<const GraphExportRegion> regions,
		std::vector<GraphExportSettings> &exports,
		std::string &failure,
		size_t imageCount = 1
	);
	bool ExportAuthoredGraphNode(
		const GraphExportSettings &grants,
		std::string_view nodeId,
		std::span<const GraphExportRegion> regions,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedTemporaryDirectories = nullptr
	);
	// Live immutable project export. An absent preview binding is added only to a bounded private copy.
	bool ExportAuthoredGraphNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::string_view nodeId,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedTemporaryDirectories = nullptr
	);

}
