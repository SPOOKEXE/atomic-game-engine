#include "ImageGraphComposerHost.hpp"

#include <engine/assets/LocalStore.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("studio.source_sdf_graph_host")
TEST_CASE("Studio routes RM graph producers to renderer receipts", "[studio][sdf-graph-host]") {
	using namespace engine;
	render::Renderer renderer;
	assets::LocalPaths paths;
	studio::detail::ImageGraphComposerHost provider(renderer, core::Name("rm.owner"), paths);
	for (const std::string type : {"pc.rm_render", "pc.rm_render_scatter", "pc.rm_cloud", "pc.rm_terrain"}) {
		DYNAMIC_SECTION(type) {
			imagegraph::Node node{"rm", type, "", {}, {}};
			imagegraph::EvaluationRequest clock;
			imagegraph::HostNodeInvocation invocation{node, clock, {}, {}, 1 << 20};
			imagegraph::HostNodeCapture receipt;
			receipt.Failure = "previous";
			std::string failure;
			CHECK_FALSE(provider.Capture(invocation, receipt, failure));
			CHECK(failure.find("renderer device") != std::string::npos);
			CHECK(receipt.Failure == "previous");
			CHECK_FALSE(provider.Pending);
			CHECK_FALSE(provider.HavePendingJobs);
		}
	}
}
