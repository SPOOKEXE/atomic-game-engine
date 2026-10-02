#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>

namespace client::detail {
	bool BuildTransformRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::TransformImage3DRequest &request,
		engine::imagegraph::Diagnostic &diagnostic
	);
}
