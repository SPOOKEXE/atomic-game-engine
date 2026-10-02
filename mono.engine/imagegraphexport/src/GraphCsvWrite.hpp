#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>
namespace engine::imagegraphexport {
	bool CaptureGraphCsvWrite(
		std::span<const GraphFileGrant>,
		const engine::assets::ContentPolicy &,
		const engine::imagegraph::HostNodeInvocation &,
		engine::imagegraph::HostNodeCapture &,
		std::string &
	);
}
