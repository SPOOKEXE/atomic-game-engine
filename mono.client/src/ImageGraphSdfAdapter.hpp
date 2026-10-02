#pragma once
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/render/SourceSdf.hpp>
namespace client::detail {
	bool BuildSdfRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		std::string_view outputPort,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::SourceSdfRequest &request,
		engine::imagegraph::Diagnostic &diagnostic,
		engine::imagegraph::HostNodeProvider *hostProvider = nullptr,
		engine::imagegraph::CapturedFeedbackHost *replayOwner = nullptr,
		uint64_t authoringRevision = 1
	);
}
