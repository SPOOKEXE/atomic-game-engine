#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

TEST_SUITE_ID("engine.scene.cameracontinuation")
TEST_DEPENDS("engine.scene.controls")
TEST_DEPENDS("engine.scene.characters")

using namespace engine;
using core::CFrame;
using core::Vector3;

namespace {
	struct Viewer {
		ecs::Store World{"camera-continuation"};
		ecs::Entity Root, Humanoid, Eye;
		Viewer() {
			scene::RegisterSceneClasses();
			scene::InstallServices(World);
			const auto model = scene::MakeCharacter(World, scene::CharacterDesc{});
			const auto rig = *World.Get<scene::Character>(model);
			Root = rig.Root;
			Humanoid = rig.Humanoid;
			Eye = World.CreateInstance(scene::CameraClass(), "Eye");
			World.Set(Eye, scene::CameraSubject{.Target = Humanoid, .Automatic = false});
			World.SetResource(scene::ActiveCamera{Eye});
			World.SetResource(scene::CameraController{});
			World.SetResource(scene::InputState{});
			World.Set(Root, scene::Transform{CFrame(Vector3{2, 3, -4})});
		}
	};

	void CheckPose(const CFrame &actual, const CFrame &expected) {
		CHECK((actual.Position - expected.Position).Magnitude() < 0.0002f);
		CHECK((actual.LookVector() - expected.LookVector()).Magnitude() < 0.00002f);
		CHECK((actual.UpVector() - expected.UpVector()).Magnitude() < 0.00002f);
		CHECK((actual.RightVector() - expected.RightVector()).Magnitude() < 0.00002f);
	}
}

TEST_CASE(
	"a humanoid camera retains roll scale and input through a copied continuation",
	"[scene][camera-continuation]"
) {
	for (const auto mode :
		 {scene::CameraMode::Classic,
		  scene::CameraMode::ShiftLock,
		  scene::CameraMode::LockFirstPerson,
		  scene::CameraMode::Scriptable}) {
		for (float scale : {0.25f, 1.0f, 3.0f}) {
			INFO("mode=" << static_cast<int>(mode) << " scale=" << scale);
			Viewer source, destination;
			source.World.GetMutable<scene::CameraSubject>(source.Eye)->Automatic =
				mode != scene::CameraMode::ShiftLock;
			REQUIRE_FALSE(source.World.Has<scene::Transform>(source.Humanoid));
			auto &control = *source.World.ResourceMutable<scene::CameraController>();
			control.Mode = mode;
			control.Angles = {0.37f, -0.62f};
			control.Basis = CFrame::Angles(-0.14f, 0.21f, 0.48f);
			control.Distance = mode == scene::CameraMode::LockFirstPerson ? 0.0f : 7.0f;
			control.OccludedDistance = mode == scene::CameraMode::LockFirstPerson ? -1.0f : 5.0f;
			control.HeadHeight = 1.7f;
			control.ShoulderOffset = 0.8f;
			if (mode == scene::CameraMode::Scriptable)
				source.World.Set(
					source.Eye,
					scene::Transform{CFrame(Vector3{11, 12, 13}) * CFrame::Angles(0.7f, -1.2f, 0.9f)}
				);
			else
				REQUIRE(scene::PlaceCamera(source.World));
			const CFrame before = source.World.Get<scene::Transform>(source.Eye)->Frame;
			const Vector3 sourceRoot = source.World.Get<scene::Transform>(source.Root)->Frame.Position;
			scene::SeamTransform seam{
				CFrame(Vector3{43, -11, 29}) * CFrame::Angles(1.1f, -0.8f, 1.3f), {4, -2, 7}, scale
			};
			auto captured = scene::CaptureCameraContinuation(source.World);
			REQUIRE(captured);
			CHECK(captured->Control.TransitSubject == ecs::NULL_ENTITY);
			REQUIRE(scene::MapCameraContinuation(*captured, seam));
			destination.World.Set(destination.Root, scene::Transform{seam.Place(CFrame(sourceRoot))});
			// An arrived transit row is already part of the copied placement, so apply
			// must establish its baseline rather than consuming that crossing twice.
			destination.World.Set(
				destination.Root, scene::PortalTransit{.Frame = seam.Frame, .Scale = scale, .Serial = 8}
			);
			REQUIRE(
				scene::ApplyCameraContinuation(
					destination.World, destination.Eye, destination.Humanoid, *captured
				)
			);
			CHECK(
				destination.World.Get<scene::CameraSubject>(destination.Eye)->Automatic == captured->Automatic
			);
			CHECK(scene::CameraSubjectRoot(destination.World, destination.Eye) == destination.Root);
			const CFrame expected = seam.Place(before);
			CheckPose(destination.World.Get<scene::Transform>(destination.Eye)->Frame, expected);
			CHECK_FALSE(scene::UpdateCameraControl(destination.World));
			CHECK(scene::PlaceCamera(destination.World) == (mode != scene::CameraMode::Scriptable));
			CheckPose(destination.World.Get<scene::Transform>(destination.Eye)->Frame, expected);
			const auto &after = *destination.World.Resource<scene::CameraController>();
			CHECK(after.Distance == Catch::Approx(control.Distance * scale));
			CHECK(
				after.OccludedDistance ==
				Catch::Approx(control.OccludedDistance < 0 ? -1.0f : control.OccludedDistance * scale)
			);
			CHECK(after.HeadHeight == Catch::Approx(control.HeadHeight * scale));
			CHECK(after.ShoulderOffset == Catch::Approx(control.ShoulderOffset * scale));
			CHECK(after.MaximumDistance == Catch::Approx(control.MaximumDistance * scale));
			CHECK(after.ZoomStep == Catch::Approx(control.ZoomStep * scale));
			CHECK(after.KeyZoomSpeed == Catch::Approx(control.KeyZoomSpeed * scale));
			CHECK(
				captured->Lens.NearPlane ==
				Catch::Approx(source.World.Get<scene::Camera>(source.Eye)->NearPlane * scale)
			);
			source.World.ResourceMutable<scene::InputState>()->Down.Set(scene::KeyCode::W, true);
			destination.World.ResourceMutable<scene::InputState>()->Down.Set(scene::KeyCode::W, true);
			const auto wanted = scene::ReadMoveIntent(source.World).Direction;
			CHECK(
				(scene::ReadMoveIntent(destination.World).Direction - seam.Rotate(wanted)).Magnitude() <
				0.00002f
			);
		}
	}
}

TEST_CASE("a replica can select a humanoid on its local camera", "[scene][camera-continuation]") {
	Viewer viewer;
	const auto camera = viewer.World.CreatePredictedInstance(scene::CameraClass(), "LocalEye");
	viewer.World.SetAdoptOnly(true);
	const core::Name subject("CameraSubject");
	CHECK_FALSE(viewer.World.SetProperty(viewer.Eye, subject, &viewer.Humanoid, sizeof(viewer.Humanoid)));
	REQUIRE(viewer.World.SetProperty(camera, subject, &viewer.Humanoid, sizeof(viewer.Humanoid)));
	CHECK(scene::CameraSubjectRoot(viewer.World, camera) == viewer.Root);
	CHECK_FALSE(viewer.World.Get<scene::CameraSubject>(camera)->Automatic);
	const auto invalid = viewer.World.CreatePredicted();
	CHECK_FALSE(viewer.World.SetProperty(camera, subject, &invalid, sizeof(invalid)));
	CHECK(viewer.World.Get<scene::CameraSubject>(camera)->Target == viewer.Humanoid);
	viewer.World.Destroy(viewer.Humanoid);
	ecs::Entity visible = viewer.Humanoid;
	REQUIRE(viewer.World.GetProperty(camera, subject, &visible, sizeof(visible)));
	CHECK(visible == ecs::NULL_ENTITY);
	CHECK(viewer.World.Get<scene::CameraSubject>(camera)->Target == viewer.Humanoid);
	const auto empty = ecs::NULL_ENTITY;
	REQUIRE(viewer.World.SetProperty(camera, subject, &empty, sizeof(empty)));
	CHECK(viewer.World.Get<scene::CameraSubject>(camera)->Target == ecs::NULL_ENTITY);
	CHECK_FALSE(viewer.World.Get<scene::CameraSubject>(camera)->Automatic);
}

TEST_CASE(
	"an explicitly cleared camera subject survives a copied continuation", "[scene][camera-continuation]"
) {
	Viewer source, destination;
	REQUIRE(scene::PlaceCamera(source.World));
	source.World.Set(source.Eye, scene::CameraSubject{.Target = ecs::NULL_ENTITY, .Automatic = false});
	auto captured = scene::CaptureCameraContinuation(source.World);
	REQUIRE(captured);
	const scene::SeamTransform seam{CFrame(Vector3{20, 10, 5}) * CFrame::Angles(.3f, -.4f, .2f), {}, 2};
	REQUIRE(scene::MapCameraContinuation(*captured, seam));
	REQUIRE(
		scene::ApplyCameraContinuation(destination.World, destination.Eye, destination.Humanoid, *captured)
	);
	CHECK(destination.World.Get<scene::CameraSubject>(destination.Eye)->Target == ecs::NULL_ENTITY);
	CHECK_FALSE(destination.World.Get<scene::CameraSubject>(destination.Eye)->Automatic);
	CHECK_FALSE(scene::PlaceCamera(destination.World));
	CheckPose(destination.World.Get<scene::Transform>(destination.Eye)->Frame, captured->Frame);
	auto inconsistent = *captured;
	inconsistent.Automatic = true;
	CHECK_FALSE(
		scene::ApplyCameraContinuation(destination.World, destination.Eye, destination.Humanoid, inconsistent)
	);
	CheckPose(destination.World.Get<scene::Transform>(destination.Eye)->Frame, captured->Frame);
	destination.World.GetMutable<scene::Humanoid>(destination.Humanoid)->RootPart = ecs::NULL_ENTITY;
	CHECK(
		scene::ApplyCameraContinuation(destination.World, destination.Eye, destination.Humanoid, *captured)
	);
}

TEST_CASE(
	"camera continuation refuses invalid maps and missing destination roots without mutation",
	"[scene][camera-continuation]"
) {
	Viewer source, destination;
	REQUIRE(scene::PlaceCamera(source.World));
	const auto captured = scene::CaptureCameraContinuation(source.World);
	REQUIRE(captured);
	for (float scale : {0.0f, -1.0f, std::numeric_limits<float>::infinity()}) {
		auto value = *captured;
		scene::SeamTransform seam;
		seam.Scale = scale;
		CHECK_FALSE(scene::MapCameraContinuation(value, seam));
		CheckPose(value.Frame, captured->Frame);
		CHECK(value.Control.Distance == captured->Control.Distance);
	}
	const CFrame previous = destination.World.Get<scene::Transform>(destination.Eye)->Frame;
	destination.World.GetMutable<scene::Humanoid>(destination.Humanoid)->RootPart = ecs::NULL_ENTITY;
	CHECK_FALSE(
		scene::ApplyCameraContinuation(destination.World, destination.Eye, destination.Humanoid, *captured)
	);
	CheckPose(destination.World.Get<scene::Transform>(destination.Eye)->Frame, previous);
}

TEST_CASE(
	"a camera consumes several cumulative portal crossings once after snapshot roundtrip",
	"[scene][camera-continuation]"
) {
	Viewer viewer;
	auto &control = *viewer.World.ResourceMutable<scene::CameraController>();
	control.Angles = {0.31f, -0.43f};
	control.Mode = scene::CameraMode::ShiftLock;
	REQUIRE(scene::PlaceCamera(viewer.World));
	CHECK_FALSE(scene::FollowPortalTransit(viewer.World));
	const CFrame startEye = viewer.World.Get<scene::Transform>(viewer.Eye)->Frame;
	const CFrame startRoot = viewer.World.Get<scene::Transform>(viewer.Root)->Frame;
	scene::SeamTransform first{
		CFrame(Vector3{7, 13, -21}) * CFrame::Angles(0.8f, 0.4f, -0.6f), {3, 5, -2}, 0.5f
	};
	scene::SeamTransform second{
		CFrame(Vector3{-4, 23, 9}) * CFrame::Angles(-0.7f, 1.2f, 0.9f), {-1, 2, 4}, 3.0f
	};
	scene::PortalTransit transit;
	for (const auto &seam : {first, second}) {
		transit.Frame = seam.Place(transit.Frame).Orthonormalize();
		transit.Scale *= seam.Scale;
		++transit.Serial;
	}
	const auto &type = ecs::Components::Describe(ecs::Components::Of<scene::PortalTransit>());
	core::ByteWriter writer;
	type.Write(writer, &transit, 1);
	core::ByteReader reader(writer.Bytes());
	scene::PortalTransit restored;
	type.Read(reader, &restored, 1);
	CHECK(std::memcmp(&restored, &transit, sizeof(transit)) == 0);
	viewer.World.Set(viewer.Root, restored);
	viewer.World.Set(viewer.Root, scene::Transform{second.Place(first.Place(startRoot))});
	REQUIRE(scene::FollowPortalTransit(viewer.World));
	CHECK_FALSE(scene::FollowPortalTransit(viewer.World));
	REQUIRE(scene::PlaceCamera(viewer.World));
	CheckPose(viewer.World.Get<scene::Transform>(viewer.Eye)->Frame, second.Place(first.Place(startEye)));
	const auto &after = *viewer.World.Resource<scene::CameraController>();
	CHECK(after.SeenTransit == 2);
	CHECK(after.Distance == Catch::Approx(18));
	CHECK(after.Angles.X == Catch::Approx(0.31f));
	// Restore the viewer's resource too, so its cumulative baseline must survive
	// a snapshot before the following relative map can be computed correctly.
	const auto &controlType = ecs::Components::Describe(ecs::Components::Of<scene::CameraController>());
	core::ByteWriter controlWriter;
	controlType.Write(controlWriter, &after, 1);
	core::ByteReader controlReader(controlWriter.Bytes());
	scene::CameraController restoredControl;
	controlType.Read(controlReader, &restoredControl, 1);
	viewer.World.SetResource(restoredControl);
	// A later crossing is relative to the nonidentity consumed baseline, not
	// relative to the world where this character was originally created.
	const scene::SeamTransform third{
		CFrame(Vector3{19, -5, 8}) * CFrame::Angles(0.4f, -0.9f, 0.6f), {2, 1, -3}, 2.0f
	};
	transit.Frame = third.Place(transit.Frame).Orthonormalize();
	transit.Scale *= third.Scale;
	++transit.Serial;
	viewer.World.Set(viewer.Root, transit);
	viewer.World.Set(viewer.Root, scene::Transform{third.Place(second.Place(first.Place(startRoot)))});
	REQUIRE(scene::FollowPortalTransit(viewer.World));
	CHECK_FALSE(scene::FollowPortalTransit(viewer.World));
	REQUIRE(scene::PlaceCamera(viewer.World));
	CheckPose(
		viewer.World.Get<scene::Transform>(viewer.Eye)->Frame,
		third.Place(second.Place(first.Place(startEye)))
	);
	CHECK(viewer.World.Resource<scene::CameraController>()->Distance == Catch::Approx(36));
	INFO(
		"CameraController bytes=" << sizeof(scene::CameraController)
								  << " PortalTransit bytes=" << sizeof(scene::PortalTransit)
	);
	CHECK(sizeof(scene::CameraController) == 144);
	CHECK(sizeof(scene::PortalTransit) == 36);
}

TEST_CASE(
	"shrinking a near zoom camera preserves its first person transition distance",
	"[scene][camera-continuation][camera-scale]"
) {
	Viewer source, destination;
	source.World.ResourceMutable<scene::CameraController>()->Distance = 0.02f;
	CHECK_FALSE(scene::UpdateCameraControl(source.World));
	REQUIRE(scene::PlaceCamera(source.World));
	auto camera = scene::CaptureCameraContinuation(source.World);
	REQUIRE(camera);
	const scene::SeamTransform seam{CFrame{}, {}, 0.25f};
	REQUIRE(scene::MapCameraContinuation(*camera, seam));
	destination.World.Set(
		destination.Root, scene::Transform{seam.Place(source.World.Get<scene::Transform>(source.Root)->Frame)}
	);
	REQUIRE(
		scene::ApplyCameraContinuation(destination.World, destination.Eye, destination.Humanoid, *camera)
	);
	CHECK_FALSE(scene::UpdateCameraControl(destination.World));
	CHECK(destination.World.Resource<scene::CameraController>()->Mode == scene::CameraMode::Classic);
	REQUIRE(scene::PlaceCamera(destination.World));
	CheckPose(destination.World.Get<scene::Transform>(destination.Eye)->Frame, camera->Frame);
}

TEST_CASE(
	"a retained camera character carries its presented pose without another simulated body",
	"[scene][camera-continuation][camera-hold]"
) {
	for (int retirement = 0; retirement < 6; ++retirement) {
		CAPTURE(retirement);
		scene::RegisterSceneClasses();
		ecs::Store authority{"camera-authority"};
		scene::InstallServices(authority);
		const auto player = scene::AddPlayer(authority, "viewer", false, 91);
		const auto model = scene::LoadCharacter(authority, player);
		const auto source = *authority.Get<scene::Character>(model);
		core::ByteWriter initial;
		REQUIRE(authority.Save(initial));
		ecs::Store replica{"camera-replica"};
		core::ByteReader joining(initial.Bytes());
		REQUIRE(replica.Apply(joining, ecs::ApplyMode::Authoritative));
		replica.SetAdoptOnly(true);
		const auto camera = replica.CreatePredictedInstance(scene::CameraClass(), "viewer");
		replica.Set(camera, scene::CameraSubject{.Target = source.Humanoid, .Automatic = false});
		replica.SetResource(scene::ActiveCamera{camera});
		replica.SetResource(scene::LocalPlayer{player});
		replica.SetResource(scene::CameraController{});
		const auto visuals = replica.CountMatching<scene::Visual>();
		const auto colliders = replica.CountMatching<scene::Collider>();
		const CFrame presented{Vector3{2, 2.5f, -4}};
		REQUIRE(scene::PrepareCameraCharacterHold(replica, player, presented));
		const auto held = *replica.Resource<scene::CameraCharacterHold>();
		REQUIRE_FALSE(scene::ActivateCameraCharacterHold(replica));
		REQUIRE(replica.CountMatching<scene::Visual>() == visuals);
		REQUIRE(replica.CountMatching<scene::Collider>() == colliders);
		REQUIRE_FALSE(replica.Has<scene::RigidBody>(held.Root));
		REQUIRE_FALSE(replica.Has<scene::Simulated>(held.Root));
		REQUIRE_FALSE(replica.Has<scene::Motion>(held.Root));
		REQUIRE(scene::PrepareCameraCharacterHold(replica, player, presented));
		REQUIRE(replica.Resource<scene::CameraCharacterHold>()->Root == held.Root);
		scene::DrawInstance bodyRow;
		bodyRow.Source = source.Root.Id;
		bodyRow.Rig = source.Root.Id;
		bodyRow.Frame = presented * CFrame(Vector3{0, 1, 0});
		bodyRow.SkinFirst = 1;
		bodyRow.SkinCount = 1;
		bodyRow.Mesh = core::Name("retained.mesh");
		std::vector<scene::DrawInstance> rows{bodyRow};
		std::vector<CFrame> joints{CFrame{}, CFrame(Vector3{1, 2, 3})};
		REQUIRE(scene::ContinueCameraBodyPose(replica, presented, rows, joints));
		REQUIRE(replica.Resource<scene::CameraBodyPose>()->Rows.size() == 1);
		auto invalidRows = rows;
		invalidRows[0].SkinFirst = static_cast<uint32_t>(joints.size() + 1);
		CHECK_FALSE(scene::ContinueCameraBodyPose(replica, presented, invalidRows, joints));
		std::vector<scene::DrawInstance> oversizedRows(257, bodyRow);
		CHECK_FALSE(scene::ContinueCameraBodyPose(replica, presented, oversizedRows, joints));
		REQUIRE(replica.Resource<scene::CameraBodyPose>()->Rows.size() == 1);
		if (retirement == 0) replica.DestroyInstance(source.Root);
		if (retirement == 1) replica.DestroyInstance(source.Humanoid);
		if (retirement == 2) replica.DestroyInstance(player);
		if (retirement == 3) replica.Remove<scene::PlayerCharacter>(player);
		if (retirement == 5) replica.DestroyInstance(model);
		if (retirement == 4) {
			REQUIRE(scene::RemoveCharacter(authority, player));
			authority.DestroyInstance(player);
			core::ByteWriter snapshot;
			REQUIRE(authority.Save(snapshot));
			core::ByteReader replacing(snapshot.Bytes());
			REQUIRE(replica.Apply(replacing, ecs::ApplyMode::Authoritative));
		}
		REQUIRE(scene::ActivateCameraCharacterHold(replica));
		const auto moved = CFrame(Vector3{5, 3, -8}) * CFrame::Angles(0, .5f, 0);
		rows.clear();
		joints.assign(2, CFrame{});
		REQUIRE(scene::ContinueCameraBodyPose(replica, moved, rows, joints));
		REQUIRE(rows.size() == 1);
		CheckPose(rows[0].Frame, moved * CFrame(Vector3{0, 1, 0}));
		CHECK(rows[0].Mesh == bodyRow.Mesh);
		CHECK(rows[0].Rig == source.Root.Id);
		CHECK(rows[0].SkinFirst == 2);
		REQUIRE(joints.size() == 3);
		CheckPose(joints[2], CFrame(Vector3{1, 2, 3}));
		core::ByteWriter savedPose;
		REQUIRE(replica.Save(savedPose));
		ecs::Store restored{"restored-pose"};
		core::ByteReader poseReader(savedPose.Bytes());
		REQUIRE(restored.Load(poseReader));
		REQUIRE(restored.Resource<scene::CameraBodyPose>());
		CHECK(restored.Resource<scene::CameraBodyPose>()->Rows.empty());
		CHECK(replica.Resource<scene::LocalPlayer>()->Instance == held.Player);
		CHECK(scene::CharacterOf(replica, held.Player) == held.Model);
		CHECK(replica.Get<scene::Character>(held.Model)->Owner == held.Player);
		CHECK(replica.Get<scene::Humanoid>(held.Humanoid)->RootPart == held.Root);
		CHECK(replica.Get<scene::CameraSubject>(camera)->Target == held.Humanoid);
		CHECK_FALSE(replica.Get<scene::CameraSubject>(camera)->Automatic);
		REQUIRE(scene::PlaceCamera(replica));
		const auto before = replica.Get<scene::Transform>(camera)->Frame;
		replica.ResourceMutable<scene::CameraController>()->Angles.Y += .2f;
		REQUIRE(scene::PlaceCamera(replica));
		CHECK(
			(replica.Get<scene::Transform>(camera)->Frame.LookVector() - before.LookVector()).Magnitude() >
			.1f
		);
		scene::ReleaseCameraCharacterHold(replica);
		CHECK_FALSE(replica.Alive(held.Player));
		CHECK_FALSE(replica.Alive(held.Model));
		CHECK_FALSE(replica.Alive(held.Root));
		CHECK_FALSE(replica.Alive(held.Humanoid));
		CHECK_FALSE(replica.HasResource<scene::CameraCharacterHold>());
		CHECK_FALSE(replica.HasResource<scene::CameraBodyPose>());
	}
}

TEST_CASE(
	"retained camera cleanup respects a newer explicit selection", "[scene][camera-continuation][camera-hold]"
) {
	for (int selection = 0; selection < 6; ++selection) {
		CAPTURE(selection);
		Viewer viewer;
		const auto player = scene::AddPlayer(viewer.World, "viewer", false, 91);
		const auto model = scene::LoadCharacter(viewer.World, player);
		const auto rig = *viewer.World.Get<scene::Character>(model);
		const auto camera = viewer.World.CreatePredictedInstance(scene::CameraClass(), "local camera");
		viewer.World.Set(camera, scene::CameraSubject{.Target = rig.Humanoid, .Automatic = false});
		viewer.World.SetResource(scene::ActiveCamera{camera});
		viewer.World.SetResource(scene::LocalPlayer{player});
		viewer.World.SetAdoptOnly(true);
		const auto target = selection % 3 == 0 ? viewer.Humanoid : ecs::NULL_ENTITY;
		if (selection >= 3)
			viewer.World.Set(camera, scene::CameraSubject{.Target = target, .Automatic = false});
		REQUIRE(scene::PrepareCameraCharacterHold(viewer.World, player, CFrame{}));
		viewer.World.Set(camera, scene::CameraSubject{.Target = target, .Automatic = false});
		if (selection % 3 == 2) {
			viewer.World.SetResource(scene::ActiveCamera{viewer.Eye});
			viewer.World.ResourceMutable<scene::CameraController>()->Mode = scene::CameraMode::Scriptable;
		}
		viewer.World.DestroyInstance(rig.Root);
		REQUIRE(scene::ActivateCameraCharacterHold(viewer.World));
		CHECK(viewer.World.Get<scene::CameraSubject>(camera)->Target == target);
		scene::ReleaseCameraCharacterHold(viewer.World);
		CHECK(viewer.World.Get<scene::CameraSubject>(camera)->Target == target);
		CHECK(
			viewer.World.Resource<scene::ActiveCamera>()->Entity == (selection % 3 == 2 ? viewer.Eye : camera)
		);
		if (selection % 3 == 2)
			CHECK(viewer.World.Resource<scene::CameraController>()->Mode == scene::CameraMode::Scriptable);
	}
}
