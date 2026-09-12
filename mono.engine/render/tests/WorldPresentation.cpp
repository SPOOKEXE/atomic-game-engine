// Device-free checks for the shared world-to-renderer presentation boundary.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
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

TEST_CASE(
	"first-person body selection follows player identity across held and native rigs", "[render][eye-body]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store authority{"body-authority"};
	scene::InstallServices(authority);
	const auto player = scene::AddPlayer(authority, "viewer", false, 91);
	const auto model = scene::LoadCharacter(authority, player);
	const auto rig = *authority.Get<scene::Character>(model);
	core::ByteWriter snapshot;
	REQUIRE(authority.Save(snapshot));
	ecs::Store replica{"body-replica"};
	core::ByteReader reader(snapshot.Bytes());
	REQUIRE(replica.Apply(reader, ecs::ApplyMode::Authoritative));
	replica.SetAdoptOnly(true);
	const auto camera = replica.CreatePredictedInstance(scene::CameraClass(), "Eye");
	replica.Set(camera, scene::CameraSubject{.Target = rig.Humanoid, .Automatic = false});
	replica.SetResource(scene::ActiveCamera{camera});
	replica.SetResource(scene::LocalPlayer{player});
	scene::CameraController controller;
	controller.Mode = scene::CameraMode::LockFirstPerson;
	replica.SetResource(controller);
	render::View view;
	render::SelectFirstPersonBody(replica, view);
	CHECK(view.EyeRig == rig.Root.Id);
	REQUIRE(view.EyePlayer == 91);
	REQUIRE(scene::PrepareCameraCharacterHold(replica, player, core::CFrame{}));
	render::ResolveEyeBody(replica, view);
	CHECK(view.EyeRig == rig.Root.Id);
	replica.DestroyInstance(rig.Humanoid);
	REQUIRE(scene::ActivateCameraCharacterHold(replica));
	const auto held = *replica.Resource<scene::CameraCharacterHold>();
	CHECK(replica.GetFullName(held.Root) != replica.GetFullName(rig.Root));
	render::SelectFirstPersonBody(replica, view);
	CHECK(view.EyeRig == rig.Root.Id);
	CHECK(view.EyePlayer == 91);
	render::ResolveEyeBody(authority, view);
	CHECK(view.EyeRig == rig.Root.Id);

	SECTION("retired source root preserves identity") {
		replica.DestroyInstance(rig.Root);
		render::SelectFirstPersonBody(replica, view);
		CHECK(view.EyeRig == held.Root.Id);
		CHECK(view.EyePlayer == 91);
		scene::CameraBodyPose retainedPose;
		retainedPose.SourceRoot = held.SourceRoot;
		replica.SetResource(std::move(retainedPose));
		render::SelectFirstPersonBody(replica, view);
		CHECK(view.EyeRig == held.SourceRoot.Id);
		CHECK(view.EyePlayer == 91);
		render::ResolveEyeBody(authority, view);
		CHECK(view.EyeRig == rig.Root.Id);
	}
	SECTION("duplicate account cannot select an arbitrary rig") {
		const auto second = scene::AddPlayer(authority, "other", false, 91);
		REQUIRE(scene::LoadCharacter(authority, second) != ecs::NULL_ENTITY);
		render::ResolveEyeBody(authority, view);
		CHECK(view.EyeRig == 0);
	}
	SECTION("changed subject clears the player identity") {
		replica.Set(camera, scene::CameraSubject{});
		render::SelectFirstPersonBody(replica, view);
		CHECK(view.EyeRig == 0);
		CHECK_FALSE(view.EyePlayer);
	}
	SECTION("third-person and scriptable cameras show the body") {
		for (const auto mode : {scene::CameraMode::Classic, scene::CameraMode::Scriptable}) {
			replica.ResourceMutable<scene::CameraController>()->Mode = mode;
			render::SelectFirstPersonBody(replica, view);
			CHECK(view.EyeRig == 0);
			CHECK_FALSE(view.EyePlayer);
		}
	}
	SECTION("unavailable player clears a previous resolution") {
		view.EyePlayer = 92;
		render::ResolveEyeBody(authority, view);
		CHECK(view.EyeRig == 0);
	}
}

TEST_CASE("eye body selection invalidates objects without changing the environment", "[render][eye-body]") {
	using namespace engine;
	std::array<scene::DrawInstance, 1> rows{};
	render::View view;
	view.Instances = rows;
	render::ScenePresentationState state;
	state.Lighting.EnvironmentState.Skybox = scene::SkyboxSource::Textures;
	state.Lighting.EnvironmentState.Textures.Enabled = true;
	state.Lighting.EnvironmentState.Textures.Front = Name("eye-body.sky");
	const auto before = render::ScenePresentationSignaturesOf(view, state);
	REQUIRE(before.Environment != 0);
	view.EyeRig = 123;
	const auto hidden = render::ScenePresentationSignaturesOf(view, state);
	CHECK(hidden.Objects != before.Objects);
	CHECK(hidden.Environment == before.Environment);
	view.EyeRig = 456;
	CHECK(render::ScenePresentationSignaturesOf(view, state).Objects != hidden.Objects);
	view.EyeRig = 0;
	const std::array<uint32_t, 1> importedHidden{0};
	view.EyeHiddenRows = importedHidden;
	const auto imported = render::ScenePresentationSignaturesOf(view, state);
	CHECK(imported.Objects != before.Objects);
	CHECK(imported.Environment == before.Environment);
}

TEST_CASE("a row edit invalidates only the camera given that edited row", "[render][presentation][damage]") {
	using namespace engine;
	std::array<scene::DrawInstance, 3> firstRows{};
	std::array<scene::DrawInstance, 3> secondRows{};
	for (size_t index = 0; index < firstRows.size(); index++) {
		firstRows[index].Source = index + 1;
		secondRows[index].Source = index + 1;
	}

	render::View first;
	first.Instances = firstRows;
	render::View second;
	second.Instances = secondRows;
	const render::ScenePresentationState state;
	const uint64_t firstBefore = render::ScenePresentationSignaturesOf(first, state).Objects;
	const uint64_t secondBefore = render::ScenePresentationSignaturesOf(second, state).Objects;
	REQUIRE(firstBefore != 0);
	REQUIRE(secondBefore != 0);

	// Camera views keep independent draw rows. The edit must reach the view
	// whose frame is being signed, while an untouched camera remains reusable.
	firstRows[1].Tint.R = 0.5f;
	CHECK(render::ScenePresentationSignaturesOf(first, state).Objects != firstBefore);
	CHECK(render::ScenePresentationSignaturesOf(second, state).Objects == secondBefore);
}

TEST_CASE("whole-eye images invalidate retained viewport composition", "[render][eye-presentation]") {
	engine::render::View view;
	view.EyeImageKey = Name("viewport-eye");
	view.WorldName = Name("viewer");
	view.World = 3;
	view.Slot = 2;
	const auto pending = engine::render::ScenePresentationSignaturesOf(view, {}).Portals;
	view.EyeImage = 7;
	view.EyeTransparentImages = {11, 13};
	const auto accepted = engine::render::ScenePresentationSignaturesOf(view, {}).Portals;
	REQUIRE(accepted != pending);
	CHECK(engine::render::ScenePresentationSignaturesOf(view, {}).Portals == accepted);
	SECTION("near transparent layer changed") {
		view.EyeTransparentImages[0] = 12;
	}
	SECTION("spatial overlay arrived") {
		view.EyeSpatialOverlayImage = 17;
	}
	SECTION("far transparent layer changed") {
		view.EyeTransparentImages[1] = 14;
	}
	SECTION("transparent layer withdrawn") {
		view.EyeTransparentImages[0] = 0;
	}
	SECTION("transparent layer order changed") {
		std::swap(view.EyeTransparentImages[0], view.EyeTransparentImages[1]);
	}
	SECTION("changed image") {
		view.EyeImage = 8;
	}
	SECTION("withdrawal") {
		view.EyeImage = 0;
	}
	SECTION("different key") {
		view.EyeImageKey = Name("other-eye");
	}
	SECTION("different world") {
		view.World = 4;
	}
	SECTION("different world name") {
		view.WorldName = Name("other-viewer");
	}
	SECTION("different viewport") {
		view.Slot = 4;
	}
	CHECK(engine::render::ScenePresentationSignaturesOf(view, {}).Portals != accepted);
}

TEST_CASE("presentation resources keep their stable saved identity", "[render][presentation]") {
	engine::render::RegisterPresentationComponents();
	engine::render::RegisterPresentationComponents();

	const engine::ecs::ComponentId id = engine::ecs::Components::Find(Name("client.DrawList"));
	REQUIRE(id.IsValid());
	CHECK(id == engine::ecs::Components::Of<engine::render::DrawList>());
}

TEST_CASE("an empty world publishes an empty reusable particle snapshot", "[render][presentation]") {
	engine::effects::RegisterEffectComponents();
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

TEST_CASE("pose-only presentation preserves static draw metadata", "[render][presentation][cache]") {
	engine::scene::RegisterSceneClasses();
	engine::render::RegisterPresentationComponents();
	engine::ecs::Store store("pose-presentation");
	store.SetResource(engine::render::DrawList{});
	const engine::ecs::Entity workspace = engine::scene::InstallServices(store);
	const engine::ecs::Entity part = engine::scene::MakePart(store, engine::scene::PartDesc{});
	REQUIRE(store.SetParent(part, workspace));
	auto visual = *store.Get<engine::scene::Visual>(part);
	visual.Tint = engine::core::Color3{0.25f, 0.5f, 0.75f};
	store.Set(part, visual);
	REQUIRE(engine::scene::SyncRendered(store) == 1);
	engine::render::CollectInstances(store);
	auto *drawList = store.ResourceMutable<engine::render::DrawList>();
	REQUIRE(drawList != nullptr);
	REQUIRE(drawList->Instances.size() == 1);
	const engine::core::Color3 tint = drawList->Instances[0].Tint;

	auto previous = *store.Get<engine::scene::PreviousTransform>(part);
	auto current = *store.Get<engine::scene::Transform>(part);
	previous.Frame.Position.X = 2.0f;
	current.Frame.Position.X = 6.0f;
	store.Set(part, previous);
	store.Set(part, current);
	store.SetFrame(1.0f / 240.0f, 0.5f);
	engine::render::CollectInstances(store);

	REQUIRE(drawList->Instances.size() == 1);
	CHECK(drawList->Instances[0].Frame.Position.X == 4.0f);
	CHECK(drawList->Instances[0].Tint == tint);
	CHECK(drawList->HasInterpolation);

	previous.Frame = current.Frame;
	store.Set(part, previous);
	engine::render::CollectInstances(store);
	CHECK(drawList->Instances[0].Frame.Position.X == 6.0f);
	CHECK(drawList->Instances[0].Tint == tint);
	CHECK_FALSE(drawList->HasInterpolation);
}

TEST_CASE("optional LOD and effect rows reach the cached draw list", "[render][presentation][lod]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::RegisterPresentationComponents();
	ecs::Store store("optional-render-state");
	store.SetResource(render::DrawList{});
	const ecs::Entity workspace = scene::InstallServices(store);
	const ecs::Entity part = scene::MakePart(store, scene::PartDesc{});
	REQUIRE(store.SetParent(part, workspace));

	scene::AutoMeshLOD automatic;
	automatic.Meshes[0] = Name("lod.auto-half");
	automatic.Meshes[1] = Name("lod.auto-quarter");
	automatic.Ratios[0] = 0.4f;
	automatic.Levels = 3;
	store.Set(part, automatic);
	scene::CustomMeshLOD custom;
	custom.Meshes[0] = Name("lod.custom-half");
	custom.Levels = 3;
	store.Set(part, custom);
	scene::RenderEffects effects;
	effects.Attachments[0].Node = Name("outline");
	effects.Attachments[0].Enabled = true;
	effects.Count = 1;
	store.Set(part, effects);

	REQUIRE(scene::SyncRendered(store) == 1);
	render::CollectInstances(store);
	auto *drawList = store.ResourceMutable<render::DrawList>();
	REQUIRE(drawList != nullptr);
	REQUIRE(drawList->Instances.size() == 1);
	CHECK(drawList->Instances[0].LodMeshes[0] == Name("lod.custom-half"));
	CHECK(drawList->Instances[0].LodMeshes[1] == Name("lod.auto-quarter"));
	CHECK(drawList->Instances[0].LodRatios[0] == 0.4f);
	CHECK(drawList->Instances[0].Effects.Attachments[0].Node == Name("outline"));

	custom.Meshes[0] = Name("lod.changed");
	custom.Meshes[1] = Name("lod.custom-quarter");
	effects.Attachments[0].Node = Name("posterise");
	store.Set(part, custom);
	store.Set(part, effects);
	render::CollectInstances(store);
	CHECK(drawList->Instances[0].LodMeshes[0] == Name("lod.changed"));
	CHECK(drawList->Instances[0].Effects.Attachments[0].Node == Name("posterise"));
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

TEST_CASE(
	"content bindings and active lens captures invalidate their rendered scene",
	"[render][presentation][damage]"
) {
	using namespace engine;
	const std::array instances{scene::DrawInstance{}};
	render::View view;
	view.Instances = instances;
	view.ContentOwner = Name("first-assets");
	std::array owners{render::WorldContentOwner{Name("foreign-world"), Name("foreign-assets")}};
	view.ForeignContentOwners = owners;
	render::ScenePresentationState state;
	const auto signature = [&] { return render::ScenePresentationSignaturesOf(view, state).Objects; };
	const auto initial = signature();
	SECTION("owner bindings change which same-name resources are drawn") {
		view.ContentOwner = Name("second-assets");
		CHECK(signature() != initial);
		view.ContentOwner = Name("first-assets");
		CHECK(signature() == initial);
		owners[0].Owner = Name("replacement-foreign-assets");
		CHECK(signature() != initial);
		owners[0].Owner = Name("foreign-assets");
		owners[0].World = Name("another-foreign-world");
		CHECK(signature() != initial);
	}
	SECTION("owner changes invalidate each resident world layer") {
		const std::array particles{render::ParticleBatch{}};
		const std::array portals{render::PortalView{}};
		view.Particles = particles;
		view.Portals = portals;
		state.Lighting.EnvironmentState.Skybox = scene::SkyboxSource::Textures;
		state.Lighting.EnvironmentState.Textures.Front = Name("owner-sky");
		const auto before = render::ScenePresentationSignaturesOf(view, state);
		const auto visibility = render::ParticleVisibilitySignature(view);
		view.ContentOwner = Name("second-assets");
		const auto after = render::ScenePresentationSignaturesOf(view, state);
		CHECK(after.Objects != before.Objects);
		CHECK(after.Environment != before.Environment);
		CHECK(after.Particles != before.Particles);
		CHECK(after.Portals != before.Portals);
		CHECK(render::ParticleVisibilitySignature(view) != visibility);
	}
	SECTION("inactive lens overrides do not animate an otherwise unchanged scene") {
		view.LensTimeSeconds = 5;
		view.LensPrograms = 18;
		view.LensContentOwner = Name("unused-lenses");
		CHECK(signature() == initial);
		view.LensTimeSeconds = 6;
		CHECK(signature() == initial);
	}
	SECTION("active lenses include captured time and effective program selection") {
		state.Lighting.ShaderLensCount = 1;
		state.Lighting.ShaderLenses[0].Shader = Name("warped-room");
		view.LensTimeSeconds = 5;
		const auto timed = signature();
		view.LensTimeSeconds = 6;
		CHECK(signature() != timed);
		view.LensTimeSeconds = 5;
		CHECK(signature() == timed);
		view.LensContentOwner = Name("other-lens-owner");
		CHECK(signature() != timed);
		view.LensPrograms = 18;
		const auto captured = signature();
		view.LensContentOwner = Name("irrelevant-authored-owner");
		CHECK(signature() == captured);
		view.LensPrograms = 19;
		CHECK(signature() != captured);
		view.LensPrograms = 18;
		state.Lighting.ShaderLenses[0].Strength = 2;
		CHECK(signature() != captured);
		view.Instances = {};
		CHECK(signature() != 0);
		view.OverrideLighting = true;
		CHECK(signature() == 0);
	}
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

TEST_CASE("fitted projection changes invalidate every present image cause", "[render][presentation][cache]") {
	engine::render::View view;
	engine::scene::DrawInstance instance;
	view.Instances = std::span(&instance, 1);
	engine::render::PortalView portal;
	view.Portals = std::span(&portal, 1);
	engine::effects::EmitterBlock block;
	engine::render::ParticleBatch particle;
	particle.Block = &block;
	view.Particles = std::span(&particle, 1);
	engine::render::ScenePresentationState state;
	state.Lighting.EnvironmentState.HasAtmosphere = true;
	const auto absent = engine::render::ScenePresentationSignaturesOf(view, state);
	view.Projection = glm::mat4{1};
	const auto fitted = engine::render::ScenePresentationSignaturesOf(view, state);
	CHECK(fitted.Objects != absent.Objects);
	CHECK(fitted.Environment != absent.Environment);
	CHECK(fitted.Particles != absent.Particles);
	CHECK(fitted.Portals != absent.Portals);
	const auto visibility = engine::render::ParticleVisibilitySignature(view);
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			view.Projection = glm::mat4{1};
			(*view.Projection)[column][row] += .125f;
			const auto changed = engine::render::ScenePresentationSignaturesOf(view, state);
			CHECK(changed.Objects != fitted.Objects);
			CHECK(changed.Environment != fitted.Environment);
			CHECK(changed.Particles != fitted.Particles);
			CHECK(changed.Portals != fitted.Portals);
			CHECK(engine::render::ParticleVisibilitySignature(view) != visibility);
		}
	}
	view.Projection = glm::mat4{1};
	(*view.Projection)[2][1] = -0.0f;
	const auto equivalent = engine::render::ScenePresentationSignaturesOf(view, state);
	CHECK(equivalent.Objects == fitted.Objects);
	CHECK(equivalent.Environment == fitted.Environment);
	CHECK(equivalent.Particles == fitted.Particles);
	CHECK(equivalent.Portals == fitted.Portals);
	CHECK(engine::render::ParticleVisibilitySignature(view) == visibility);
}

TEST_CASE(
	"imported portal generations and entrance bindings invalidate the retained portal image",
	"[render][presentation][cache]"
) {
	engine::render::View view;
	engine::render::PortalView portal;
	view.Portals = std::span(&portal, 1);
	const auto local = engine::render::ScenePresentationSignaturesOf(view, {});
	portal.ExternalImage = true;
	const auto unavailable = engine::render::ScenePresentationSignaturesOf(view, {});
	CHECK(unavailable.Portals != local.Portals);
	portal.ImportedImage = 123;
	portal.ImagePortal = Name("import.entrance");
	const auto accepted = engine::render::ScenePresentationSignaturesOf(view, {});
	CHECK(accepted.Portals != unavailable.Portals);
	CHECK(engine::render::ScenePresentationSignaturesOf(view, {}).Portals == accepted.Portals);
	portal.ImportedImage++;
	CHECK(engine::render::ScenePresentationSignaturesOf(view, {}).Portals != accepted.Portals);
	portal.ImportedImage--;
	portal.ImagePortal = Name("import.replaced-entrance");
	CHECK(engine::render::ScenePresentationSignaturesOf(view, {}).Portals != accepted.Portals);
	CHECK(accepted.Objects == 0);
	CHECK(accepted.Environment == 0);
	CHECK(accepted.Particles == 0);
}

TEST_CASE(
	"request mirrors derive slots without an active camera or authored camera mutations",
	"[render][presentation][surface-slots]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("request-mirrors");
	const auto workspace = scene::InstallServices(store);
	std::array<ecs::Entity, 2> cameras, panes;
	for (size_t index = 0; index < cameras.size(); ++index) {
		scene::PartDesc part;
		part.Frame.Position = {float(index) * 4, 0, -4};
		part.Size = {2, 2, .1f};
		panes[index] = scene::MakePart(store, part);
		REQUIRE(store.SetParent(panes[index], workspace));
		cameras[index] = store.CreateInstance(ecs::Classes::Find(Name("SurfaceCamera")), "Mirror");
		REQUIRE(store.SetParent(cameras[index], panes[index]));
		auto surface = *store.Get<scene::SurfaceCamera>(cameras[index]);
		surface.Surface = -1;
		store.Set(cameras[index], surface);
	}
	render::View viewer;
	viewer.CameraFrame.Position = {0, 0, 2};
	std::vector<render::SurfaceView> surfaces;
	REQUIRE(render::CollectSurfaceViews(store, surfaces, {}, &viewer) == 2);
	CHECK(surfaces[0].Index == 0);
	CHECK(surfaces[1].Index == 1);
	for (size_t index = 0; index < cameras.size(); ++index) {
		CHECK(store.Get<scene::SurfaceCamera>(cameras[index])->Surface == -1);
		CHECK(store.Get<scene::Transform>(cameras[index])->Frame.Position == core::Vector3{});
		CHECK(surfaces[index].Frame.Position.Z < -4);
	}
	CHECK(store.Resource<scene::ActiveCamera>() == nullptr);
}

TEST_CASE(
	"request surface slots share scene ordering and preserve foreign rows",
	"[render][presentation][surface-slots]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("request-slot-map");
	const auto workspace = scene::InstallServices(store);
	scene::PartDesc part;
	part.Frame.Position = {0, 0, -4};
	const auto pane = scene::MakePart(store, part);
	part.Frame.Position = {0, 0, -12};
	const auto destination = scene::MakePart(store, part);
	REQUIRE(store.SetParent(pane, workspace));
	REQUIRE(store.SetParent(destination, workspace));
	const auto cameraClass = ecs::Classes::Find(core::Name("SurfaceCamera"));
	const auto first = store.CreateInstance(cameraClass, "First");
	const auto second = store.CreateInstance(cameraClass, "Second");
	REQUIRE(store.SetParent(first, pane));
	REQUIRE(store.SetParent(second, pane));
	for (const auto camera : {first, second}) {
		auto surface = *store.Get<scene::SurfaceCamera>(camera);
		surface.Surface = -1;
		store.Set(camera, surface);
		scene::Portal portal;
		portal.Destination = destination;
		store.Set(camera, portal);
	}
	std::vector<scene::SurfaceSlot> slots;
	REQUIRE(scene::GatherSurfaceSlots(store, slots) == 2);
	REQUIRE(slots[0].Camera == first);
	REQUIRE(slots[1].Camera == second);
	std::vector<render::PortalView> portals;
	REQUIRE(render::CollectPortalViews(store, portals, slots) == 2);
	CHECK(portals[0].Index != portals[1].Index);
	CHECK(portals[0].Index >= 0);
	CHECK(portals[1].Index >= 0);
	std::array<scene::DrawInstance, 3> instances{};
	for (auto &instance : instances) {
		instance.Source = pane.Id;
		instance.Surface = -1;
	}
	instances[1].SourceWorld = core::Name("foreign-owner");
	instances[2].SourceWorld = core::Name("request-slot-map");
	render::ApplySurfaceSlots(instances, slots, core::Name("request-slot-map"));
	CHECK(instances[0].Surface == 1);
	CHECK(instances[1].Surface == -1);
	CHECK(instances[2].Surface == 1);
	CHECK(store.Get<scene::SurfaceCamera>(first)->Surface == -1);
	CHECK(store.Get<scene::SurfaceCamera>(second)->Surface == -1);
}
