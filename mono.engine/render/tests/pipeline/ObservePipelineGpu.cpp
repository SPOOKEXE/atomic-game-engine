#include "../RenderFixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.render.observepipeline")
TEST_DEPENDS("engine.graph.enginegraph")

TEST_CASE("an observed pipeline reports every node of its last finished frame", "[render][gpu]") {
	using namespace engine;
	using namespace engine::render;
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	SceneTarget target{16, 16};
	View view;
	view.World = 3;
	view.WorldName = core::Name("observed-world");
	view.Target = &target;
	OverlayImage overlay;
	const auto frame = [&] { renderer.Render(std::span(&view, 1), overlay, nullptr, false); };

	frame();
	CHECK(renderer.ObservedRun().Nodes.empty());
	const auto identity = renderer.ResolvePipelineIdentity(view.Pipeline);
	REQUIRE(identity.has_value());
	const core::Name pipeline = identity->Name;
	REQUIRE(pipeline.IsValid());
	renderer.ObservePipeline(pipeline);
	frame();
	// A frame publishes when the next one starts.
	CHECK(renderer.ObservedRun().Nodes.empty());
	frame();
	const auto &run = renderer.ObservedRun();
	CHECK(run.Pipeline == pipeline);
	REQUIRE_FALSE(run.Nodes.empty());
	for (const auto &node : run.Nodes) {
		CHECK(node.Status == ang::NodeStatus::Ran);
		CHECK_FALSE(node.Node.Text().starts_with("render."));
		CHECK(node.BeginMilliseconds >= 0.0);
		CHECK(node.BeginMilliseconds + node.Milliseconds <= run.Milliseconds);
	}
	renderer.ObservePipeline({});
	frame();
	CHECK(renderer.ObservedRun().Nodes.empty());
}
