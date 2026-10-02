#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>
namespace engine::imagegraphexport {
	bool IsGraphSpriteHost(std::string_view type);
	bool CaptureGraphSprite(
		const engine::imagegraph::HostNodeInvocation &invocation,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	);
}
