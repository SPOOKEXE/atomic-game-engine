#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>

namespace engine::imagegraphexport {
	// File grants cover one model import or an OBJ export and its bounded material sidecars.
	bool CaptureGraphMeshFile(
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		const engine::imagegraph::HostNodeInvocation &invocation,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	);
}
