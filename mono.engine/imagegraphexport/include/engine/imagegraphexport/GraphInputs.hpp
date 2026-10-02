#pragma once
#include <engine/imagegraphexport/GraphExport.hpp>
namespace engine::imagegraphexport {
	bool LoadGraphImageInputs(
		const GraphExportSettings &settings,
		std::vector<engine::imagegraph::RequestImageSource> &sources,
		std::string &failure,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
