// Device-free checks for the shared world-to-renderer presentation boundary.

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

TEST_SUITE_ID("engine.render.worldpresentation")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.render.passes")
TEST_DEPENDS("engine.scene.services")

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

TEST_CASE("fully transparent parts never enter the resident draw list", "[render][presentation]") {
	engine::scene::RegisterSceneClasses();
	engine::render::RegisterPresentationComponents();
	engine::ecs::Store store("transparent-presentation");
	store.SetResource(engine::render::DrawList{});
	const engine::ecs::Entity workspace = engine::scene::InstallServices(store);

	const auto part = [&](float authored, float local) {
		const engine::ecs::Entity entity = engine::scene::MakePart(store, engine::scene::PartDesc{});
		REQUIRE(entity != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(entity, workspace));
		auto visual = *store.Get<engine::scene::Visual>(entity);
		visual.Transparency = authored;
		store.Set(entity, visual);
		store.Set(entity, engine::scene::LocalTransparency{local});
		return entity;
	};

	const engine::ecs::Entity opaque = part(0.0f, 0.0f);
	const engine::ecs::Entity blended = part(0.999f, 0.0f);
	part(1.0f, 0.0f);
	part(2.0f, 0.0f);
	part(0.0f, 1.0f);

	REQUIRE(engine::scene::SyncRendered(store) == 5);
	engine::render::CollectInstances(store);
	auto *drawList = store.ResourceMutable<engine::render::DrawList>();
	REQUIRE(drawList != nullptr);
	REQUIRE(drawList->Instances.size() == 2);
	CHECK(std::count_if(drawList->Instances.begin(), drawList->Instances.end(), [opaque](const auto &row) {
			  return row.Source == opaque.Id;
		  }) == 1);
	CHECK(std::count_if(drawList->Instances.begin(), drawList->Instances.end(), [blended](const auto &row) {
			  return row.Source == blended.Id;
		  }) == 1);

	// A reused draw list may hold last frame's palette. Fresh unrigged rows reset
	// their own offsets, and the no-Skeleton fast path must still clear the pool.
	drawList->Instances[0].SkinFirst = 4;
	drawList->Instances[0].SkinCount = 2;
	drawList->JointFrames.push_back(engine::core::CFrame{});
	store.Set(opaque, engine::scene::LocalTransparency{});
	engine::render::CollectInstances(store);
	CHECK(drawList->Instances[0].SkinFirst == 0);
	CHECK(drawList->Instances[0].SkinCount == 0);
	CHECK(drawList->JointFrames.empty());
}

TEST_CASE("irrelevant transform writes do not hide visible source changes", "[render][presentation][cache]") {
	engine::scene::RegisterSceneClasses();
	engine::render::RegisterPresentationComponents();
	engine::ecs::Store store("cached-presentation");
	store.SetResource(engine::render::DrawList{});
	const engine::ecs::Entity workspace = engine::scene::InstallServices(store);
	const engine::ecs::Entity visible = engine::scene::MakePart(store, engine::scene::PartDesc{});
	REQUIRE(store.SetParent(visible, workspace));
	REQUIRE(engine::scene::SyncRendered(store) == 1);

	engine::render::CollectInstances(store);
	auto *drawList = store.ResourceMutable<engine::render::DrawList>();
	REQUIRE(drawList != nullptr);
	store.ClearChanges();

	// A transform outside Workspace has no Rendered tag. Its motion must leave
	// the visible result alone without masking the next visible write.
	const engine::ecs::Entity hidden = engine::scene::MakePart(store, engine::scene::PartDesc{});
	REQUIRE(hidden != engine::ecs::NULL_ENTITY);
	auto placement = *store.Get<engine::scene::Transform>(hidden);
	placement.Frame.Position.X = 4.0f;
	store.Set(hidden, placement);
	engine::render::CollectInstances(store);
	REQUIRE(drawList->Instances.size() == 1);
	CHECK(drawList->Instances.front().Source == visible.Id);

	auto visual = *store.Get<engine::scene::Visual>(visible);
	visual.Tint.R = 0.25f;
	store.Set(visible, visual);
	engine::render::CollectInstances(store);
	REQUIRE(drawList->Instances.size() == 1);
	CHECK(drawList->Instances.front().Tint.R == 0.25f);
}

TEST_CASE("a draw list flattens each rig palette beside its instance", "[render][presentation][skinning]") {
	engine::scene::RegisterSceneClasses();
	engine::render::RegisterPresentationComponents();
	engine::ecs::Store store("skinned-presentation");
	store.SetResource(engine::render::DrawList{});
	const engine::ecs::Entity workspace = engine::scene::InstallServices(store);
	const engine::ecs::Entity rig = engine::scene::MakePart(store, engine::scene::PartDesc{});
	REQUIRE(store.SetParent(rig, workspace));
	store.Set(rig, engine::scene::Skeleton{Name("presentation.Rig"), 2, {}});

	for (uint16_t joint = 0; joint < 2; joint++) {
		const engine::ecs::Entity bone =
			store.CreateInstance(engine::scene::BoneClass(), joint == 0 ? "Root" : "Child");
		REQUIRE(store.SetParent(bone, rig));
		engine::scene::Bone pose;
		pose.Joint = joint;
		pose.ParentJoint = engine::scene::NO_JOINT;
		pose.WorldFrame = engine::core::CFrame(engine::core::Vector3(static_cast<float>(joint + 1), 0, 0));
		store.Set(bone, pose);
	}

	REQUIRE(engine::scene::SyncRendered(store) == 1);
	engine::render::CollectInstances(store);
	const auto *drawList = store.Resource<engine::render::DrawList>();
	REQUIRE(drawList != nullptr);
	REQUIRE(drawList->Instances.size() == 1);
	CHECK(drawList->Instances[0].SkinFirst == 0);
	CHECK(drawList->Instances[0].SkinCount == 2);
	REQUIRE(drawList->JointFrames.size() == 2);
	CHECK(drawList->JointFrames[0].Position.X == 1.0f);
	CHECK(drawList->JointFrames[1].Position.X == 2.0f);
}

TEST_CASE("copied skin palettes are rebased and malformed runs are disabled", "[render][skinning]") {
	std::array<engine::scene::DrawInstance, 2> instances{};
	instances[0].SkinFirst = 1;
	instances[0].SkinCount = 2;
	instances[1].SkinFirst = 3;
	instances[1].SkinCount = 2;
	const std::array source{
		engine::core::CFrame(engine::core::Vector3{1, 0, 0}),
		engine::core::CFrame(engine::core::Vector3{2, 0, 0}),
		engine::core::CFrame(engine::core::Vector3{3, 0, 0}),
	};
	std::vector<engine::core::CFrame> destination{engine::core::CFrame(engine::core::Vector3{9, 0, 0})};

	engine::render::RebaseSkinPalettes(instances, source, destination);

	CHECK(instances[0].SkinFirst == 1);
	CHECK(instances[0].SkinCount == 2);
	CHECK(instances[1].SkinFirst == 0);
	CHECK(instances[1].SkinCount == 0);
	REQUIRE(destination.size() == 3);
	CHECK(destination[1].Position.X == 2.0f);
	CHECK(destination[2].Position.X == 3.0f);
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

TEST_CASE("a changed joint palette invalidates scene pixels", "[render][presentation][skinning]") {
	engine::scene::DrawInstance instance;
	instance.SkinCount = 1;
	const std::array instances{instance};
	std::array joints{engine::core::CFrame{}};
	engine::render::View view;
	view.Instances = instances;
	view.JointFrames = joints;
	const uint64_t original = engine::render::ScenePresentationSignature(view, {});

	joints[0].Position.X = 1.0f;
	CHECK(engine::render::ScenePresentationSignature(view, {}) != original);
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

TEST_CASE(
	"particle visibility ignores simulation time but notices camera and resident changes",
	"[render][presentation][cache]"
) {
	engine::render::View view;
	engine::effects::EmitterBlock block;
	engine::render::ParticleBatch batch;
	batch.Block = &block;
	const std::array particles{batch};
	view.Particles = particles;

	const uint64_t original = engine::render::ParticleVisibilitySignature(view);
	view.ParticleRevision++;
	CHECK(engine::render::ParticleVisibilitySignature(view) == original);

	view.CameraFrame.Position.X = 1.0f;
	CHECK(engine::render::ParticleVisibilitySignature(view) != original);
	view.CameraFrame.Position.X = 0.0f;
	view.ParticleResidentRevision++;
	CHECK(engine::render::ParticleVisibilitySignature(view) != original);
}
