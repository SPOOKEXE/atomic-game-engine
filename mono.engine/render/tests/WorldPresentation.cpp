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
	engine::scene::DrawInstance instance;
	const std::array instances{instance};
	engine::render::View view;
	view.Instances = instances;
	engine::render::ScenePresentationState state;
	const uint64_t original = engine::render::ScenePresentationSignature(view, state);

	view.CameraFrame.Position.X = 1.0f;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);

	view.CameraFrame.Position.X = 0.0f;
	state.Untextured = true;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);
}

TEST_CASE("absent object layer cannot invalidate scene pixels", "[render][presentation][damage]") {
	engine::render::View view;
	engine::render::ScenePresentationState state;
	CHECK(engine::render::ScenePresentationSignaturesOf(view, state).Objects == 0);

	view.CameraFrame.Position.X = 42.0f;
	view.World = 91;
	state.Animation = 12;
	state.Resources = 34;
	state.Untextured = true;
	state.Lighting.Ambient = {0.2f, 0.4f, 0.6f};
	CHECK(engine::render::ScenePresentationSignaturesOf(view, state).Objects == 0);
}

TEST_CASE("only selected environment state invalidates scene pixels", "[render][presentation][damage]") {
	engine::render::View view;
	engine::render::ScenePresentationState state;
	const uint64_t original = engine::render::ScenePresentationSignature(view, state);

	state.Lighting.EnvironmentState.CloudLayer.Cover = 0.9f;
	state.Lighting.EnvironmentState.Air.Density = 0.8f;
	CHECK(engine::render::ScenePresentationSignature(view, state) == original);

	state.Lighting.EnvironmentState.HasAtmosphere = true;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);

	state = {};
	state.Lighting.EnvironmentState.Textures.Front = Name("unused.atex");
	CHECK(engine::render::ScenePresentationSignature(view, state) == original);
	state.Lighting.EnvironmentState.Skybox = engine::scene::SkyboxSource::Textures;
	CHECK(engine::render::ScenePresentationSignature(view, state) != original);

	state.Lighting.EnvironmentState.Textures.Enabled = false;
	const uint64_t disabled = engine::render::ScenePresentationSignature(view, state);
	state.Lighting.EnvironmentState.Textures.Front = Name("still-unused.atex");
	CHECK(engine::render::ScenePresentationSignature(view, state) == disabled);

	state = {};
	state.Lighting.EnvironmentState.HasAtmosphere = true;
	state.Lighting.EnvironmentState.HasAtmosphereCompute = true;
	state.Lighting.EnvironmentState.AirCompute.Enabled = false;
	const uint64_t authoredAtmosphere = engine::render::ScenePresentationSignature(view, state);
	state.Lighting.EnvironmentState.AirCompute.Shader = engine::scene::AtmosphereProceduralShader::Alien;
	CHECK(engine::render::ScenePresentationSignature(view, state) == authoredAtmosphere);
	state.Lighting.EnvironmentState.AirCompute.Enabled = true;
	CHECK(engine::render::ScenePresentationSignature(view, state) != authoredAtmosphere);

	state = {};
	state.Lighting.EnvironmentState.HasClouds = true;
	state.Lighting.EnvironmentState.HasCloudCompute = true;
	state.Lighting.EnvironmentState.CloudVolume.Enabled = false;
	const uint64_t authoredClouds = engine::render::ScenePresentationSignature(view, state);
	state.Lighting.EnvironmentState.CloudVolume.Shader = engine::scene::CloudComputeShader::Voxel;
	CHECK(engine::render::ScenePresentationSignature(view, state) == authoredClouds);
	state.Lighting.EnvironmentState.CloudVolume.Enabled = true;
	CHECK(engine::render::ScenePresentationSignature(view, state) != authoredClouds);
}

TEST_CASE("empty Lighting has no retained environment layer", "[render][presentation][cache]") {
	engine::scene::DrawInstance instance;
	const std::array instances{instance};
	engine::render::View view;
	view.Instances = instances;
	engine::render::ScenePresentationState state;
	CHECK_FALSE(engine::render::EnvironmentLayerPresent(state.Lighting));
	CHECK(engine::render::ScenePresentationSignaturesOf(view, state).Environment == 0);

	state.Lighting.Ambient.R = 0.75f;
	const engine::render::ScenePresentationSignatures changed =
		engine::render::ScenePresentationSignaturesOf(view, state);
	CHECK(changed.Environment == 0);
	CHECK(changed.Objects != engine::render::ScenePresentationSignaturesOf(view, {}).Objects);
}

TEST_CASE("only enabled selected providers create an environment layer", "[render][presentation][cache]") {
	engine::scene::WorldLighting lighting;
	lighting.EnvironmentState.Skybox = engine::scene::SkyboxSource::Textures;
	CHECK_FALSE(engine::render::EnvironmentLayerPresent(lighting));

	lighting.EnvironmentState.Textures.Front = Name("front.atex");
	CHECK(engine::render::EnvironmentLayerPresent(lighting));

	lighting = {};
	lighting.EnvironmentState.HasAtmosphere = true;
	CHECK(engine::render::EnvironmentLayerPresent(lighting));
	lighting.EnvironmentState.HasAtmosphereCompute = true;
	lighting.EnvironmentState.AirCompute.Enabled = false;
	CHECK_FALSE(engine::render::EnvironmentLayerPresent(lighting));

	lighting = {};
	lighting.EnvironmentState.HasClouds = true;
	lighting.EnvironmentState.CloudLayer.Enabled = false;
	CHECK_FALSE(engine::render::EnvironmentLayerPresent(lighting));
	lighting.EnvironmentState.CloudLayer.Enabled = true;
	lighting.EnvironmentState.HasCloudCompute = true;
	lighting.EnvironmentState.CloudVolume.Enabled = false;
	CHECK_FALSE(engine::render::EnvironmentLayerPresent(lighting));
}

TEST_CASE("scene cache causes are signed independently", "[render][presentation][cache]") {
	engine::render::View view;
	engine::effects::EmitterBlock block;
	engine::render::ParticleBatch batch;
	batch.Block = &block;
	const std::array particlesInView{batch};
	engine::render::ScenePresentationState state;
	const engine::render::ScenePresentationSignatures original =
		engine::render::ScenePresentationSignaturesOf(view, state);

	view.Particles = particlesInView;
	view.ParticleRevision++;
	const engine::render::ScenePresentationSignatures particles =
		engine::render::ScenePresentationSignaturesOf(view, state);
	CHECK(particles.Objects == original.Objects);
	CHECK(particles.Particles != original.Particles);
	CHECK(particles.Environment == original.Environment);
	CHECK(particles.Portals == original.Portals);

	view.Particles = {};
	view.ParticleRevision--;
	state.Lighting.EnvironmentState.HasAtmosphere = true;
	const engine::render::ScenePresentationSignatures environment =
		engine::render::ScenePresentationSignaturesOf(view, state);
	CHECK(environment.Objects == original.Objects);
	CHECK(environment.Particles == original.Particles);
	CHECK(environment.Environment != original.Environment);
	CHECK(environment.Portals == original.Portals);
}
