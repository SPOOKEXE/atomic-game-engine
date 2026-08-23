// Device-free checks for the shared world-to-renderer presentation boundary.

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

TEST_SUITE_ID("engine.render.worldpresentation")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.render.passes")

using engine::core::Name;

TEST_CASE("presentation resources keep their stable saved identity", "[render][presentation]") {
	engine::render::RegisterPresentationComponents();
	engine::render::RegisterPresentationComponents();

	const engine::ecs::ComponentId id = engine::ecs::Components::Find(Name("client.DrawList"));
	REQUIRE(id.IsValid());
	CHECK(id == engine::ecs::Components::Of<engine::render::DrawList>());
}

TEST_CASE("an empty world publishes an empty reusable particle snapshot", "[render][presentation]") {
	engine::ecs::Store store("empty-presentation");
	engine::render::ParticleFrame frame;
	frame.Pool = 64;
	frame.BlockCount = 2;

	CHECK(engine::render::CollectParticleBatches(store, frame) == 0);
	CHECK(frame.SourceWorld == Name("empty-presentation"));
	CHECK(frame.Revision == 1);
	CHECK(frame.Pool == 0);
	CHECK(frame.BlockCount == 0);

	CHECK(engine::render::CollectParticleBatches(store, frame) == 0);
	CHECK(frame.Revision == 1);

	frame.Clear();
	CHECK_FALSE(frame.SourceWorld.IsValid());
	CHECK(engine::render::CollectParticleBatches(store, frame) == 0);
	CHECK(frame.Revision == 2);
}

TEST_CASE("world pipeline selection is qualified and replaced in one engine cache", "[render][pipeline]") {
	engine::graph::PipelineSet first;
	REQUIRE(first.Set(Name("main"), engine::graph::DefaultPbrDocument()));
	REQUIRE(first.Set(Name("reflection"), engine::graph::DefaultPbrDocument()));

	engine::render::Renderer renderer;
	CHECK(
		engine::render::InstallWorldPipeline(first, renderer, 17, Name("reflection")) == Name("reflection#17")
	);
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("reflection#17")});

	engine::graph::PipelineSet replacement;
	REQUIRE(replacement.Set(Name("cinematic"), engine::graph::DefaultPbrDocument()));
	CHECK(
		engine::render::InstallWorldPipeline(replacement, renderer, 17, Name("cinematic")) ==
		Name("cinematic#17")
	);
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("cinematic#17")});
}

TEST_CASE("scene presentation signature excludes viewport geometry", "[render][presentation][damage]") {
	engine::scene::DrawInstance instance;
	instance.Source = 7;
	const std::array instances{instance};

	engine::render::View view;
	view.Instances = instances;
	view.World = 9;
	view.WorldName = Name("signature-world");
	const engine::render::ScenePresentationState state;
	const uint64_t scene = engine::render::ScenePresentationSignature(view, state);

	CHECK(scene == engine::render::ScenePresentationSignature(view, state));
	CHECK(
		engine::render::ViewportPresentationSignature(800, 600) !=
		engine::render::ViewportPresentationSignature(801, 600)
	);
	CHECK(scene == engine::render::ScenePresentationSignature(view, state));
}

TEST_CASE("camera and renderer state invalidate scene pixels", "[render][presentation][damage]") {
	engine::render::View view;
	engine::render::ScenePresentationState state;
	const uint64_t original = engine::render::ScenePresentationSignature(view, state);

	view.CameraFrame.Position.X = 1.0f;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);

	view.CameraFrame.Position.X = 0.0f;
	state.Untextured = true;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);
}

TEST_CASE(
	"only lighting consumed by render passes invalidates scene pixels", "[render][presentation][damage]"
) {
	engine::render::View view;
	engine::render::ScenePresentationState state;
	const uint64_t original = engine::render::ScenePresentationSignature(view, state);

	state.Lighting.Sky.Enabled = !state.Lighting.Sky.Enabled;
	state.Lighting.Sky.Cover = 0.9f;
	state.Lighting.Air.Density = 0.8f;
	CHECK(engine::render::ScenePresentationSignature(view, state) == original);

	state.Lighting.Ambient.R = 0.25f;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);
}
