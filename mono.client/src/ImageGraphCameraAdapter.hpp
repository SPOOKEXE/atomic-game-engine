#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/render/SourceCamera3D.hpp>

namespace engine::imagegraphfont {
	class GraphFontInputs;
}

namespace client::detail {
	bool BuildCameraRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		std::string_view outputPort,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::SourceCamera3DRequest &request,
		engine::imagegraph::Diagnostic &diagnostic,
		engine::imagegraph::HostNodeProvider *hostProvider = nullptr,
		engine::imagegraph::CapturedFeedbackHost *replayOwner = nullptr,
		uint64_t authoringRevision = 1,
		const engine::imagegraphfont::GraphFontInputs *fonts = nullptr,
		const engine::imagegraph::EvaluationRequest *fontInputs = nullptr
	);
}
