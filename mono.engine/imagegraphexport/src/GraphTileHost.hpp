#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>
namespace engine::imagegraphexport {
	bool CaptureGraphTileFile(
		const engine::imagegraph::HostNodeInvocation &,
		std::span<const GraphFileGrant>,
		const engine::assets::ContentPolicy &,
		engine::imagegraph::HostNodeCapture &,
		std::string &
	);
}
