#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>
namespace engine::imagegraphexport {
	bool IsGraphTextFileHost(std::string_view type);
	bool CaptureGraphTextFile(
		std::span<const GraphFileGrant>,
		const engine::assets::ContentPolicy &,
		const engine::imagegraph::HostNodeInvocation &,
		engine::imagegraph::HostNodeCapture &,
		std::string &
	);
}
