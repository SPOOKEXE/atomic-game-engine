#include "ImageGraphComposerAdapter.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("client.source_sdf_graph_host")
TEST_CASE(
	"client routes RM graph producers through live and snapshot device hosts", "[client][sdf-graph-host]"
) {
	using namespace engine;
	for (const std::string type : {"pc.rm_render", "pc.rm_render_scatter", "pc.rm_cloud", "pc.rm_terrain"}) {
		for (const bool live : {false, true}) {
			DYNAMIC_SECTION(type << " live=" << live) {
				imagegraph::Node node{"rm", type, "", {}, {}};
				imagegraph::EvaluationRequest clock;
				imagegraph::HostNodeInvocation invocation{node, clock, {}, {}, 1 << 20};
				render::Renderer renderer;
				client::detail::ComposerProvider provider(
					renderer, core::Name("rm.owner"), nullptr, live ? core::Name("rm.binding") : core::Name{}
				);
				imagegraph::HostNodeCapture receipt;
				receipt.Failure = "previous";
				std::string failure;
				CHECK_FALSE(provider.Capture(invocation, receipt, failure));
				CHECK(failure.find("device") != std::string::npos);
				CHECK(receipt.Failure == "previous");
				CHECK_FALSE(provider.Pending);
			}
		}
	}
}
