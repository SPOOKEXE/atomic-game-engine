#pragma once

#include <engine/imagegraphexport/GraphImageCache.hpp>

namespace engine::imagegraphexport {
	bool ReadGraphSavedImageCache(
		const imagegraph::HostNodeInvocation &invocation,
		std::span<const GraphImageCacheLayoutObservation> observations,
		std::vector<imagegraph::Image> &frames,
		bool &enabled,
		std::string &failure
	);
}
