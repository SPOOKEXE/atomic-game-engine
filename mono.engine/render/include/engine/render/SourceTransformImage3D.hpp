#pragma once
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>

namespace engine::render::imagegraph {
	// Uses already resolved inputs. No graph evaluation or GPU submission occurs here.
	bool BuildSourceTransformImage3DRequest(
		const engine::imagegraph::HostNodeInvocation &invocation,
		TransformImage3DRequest &request,
		engine::imagegraph::MeshValue3D &mesh,
		std::string &failure
	);
}
