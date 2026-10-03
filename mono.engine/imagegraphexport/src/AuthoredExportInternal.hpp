#pragma once
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
namespace engine::imagegraphexport::detail {
	bool PlanPreparedAuthoredExport(
		const engine::imagegraph::Document &,
		const engine::imagegraph::Node &,
		const engine::imagegraph::EvaluationSnapshot &,
		const engine::imagegraph::EvaluationRequest &,
		const GraphExportSettings &,
		std::span<const GraphExportRegion>,
		std::vector<GraphExportSettings> &,
		size_t &imageCount,
		double &exportType,
		std::string &failure
	);
	bool ExportBatch(
		std::span<GraphExportSettings>,
		const engine::imagegraph::Document &,
		const engine::imagegraph::Plan &,
		const engine::imagegraph::EvaluationRequest &,
		bool sequenceFrames,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedDirectories,
		void (*selectTarget)(void *, size_t) = nullptr,
		void *targetContext = nullptr
	);
}
