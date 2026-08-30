#include "CustomNode.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.customnodes")
TEST_DEPENDS("engine.graph.pipelinecatalogue")

namespace engine::render::tests {

	TEST_CASE("a custom kind and native handler share one registration path", "[render][custom]") {
		graph::RegisterRenderNodeKinds();
		Renderer renderer;
		const auto state = examples::InstallCustomNode(renderer);
		REQUIRE(state != nullptr);
		CHECK_FALSE(state->DeviceReady);

		const core::Name kind("example-marker");
		const graph::NodeKindSpec *spec = graph::NodeCatalogue::Find(kind);
		REQUIRE(spec != nullptr);
		CHECK(spec->Queue == graph::ExecutionQueue::Cpu);
		CHECK_FALSE(spec->BuiltInBackend);

		graph::RenderGraph pipeline;
		const graph::ResourceId image = pipeline.AddResource({
			.Name = core::Name("image"),
			.Kind = graph::ResourceKind::Texture,
			.External = true,
		});
		graph::Node marker;
		marker.Name = core::Name("marker");
		marker.Kind = kind;
		marker.Reads = {image};
		marker.Scope = graph::NodeScope::Frame;
		REQUIRE(pipeline.AddNode(marker).IsValid());

		CHECK(renderer.SetPipeline(core::Name("custom#1"), pipeline));
	}

	TEST_CASE("the custom handler door does not replace built-ins", "[render][custom]") {
		graph::RegisterRenderNodeKinds();
		Renderer renderer;
		CHECK_FALSE(renderer.InstallNodeHandler({}, [](const graph::RunContext &) { return true; }));
		CHECK_FALSE(renderer.InstallNodeHandler(core::Name("gbuffer"), [](const graph::RunContext &) {
			return true;
		}));
	}
}
