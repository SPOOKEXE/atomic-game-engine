#include <engine/scene/CameraPortalView.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.scene.cameraportalview")

using namespace engine;

TEST_CASE("receiving mouth rounding does not recross a moving eye", "[scene][camera-portal-arrival]") {
	scene::PortalSeam entrance;
	entrance.Normal = core::Vector3::ZAxis;
	entrance.First = {2, 0, 0};
	entrance.Second = {0, 2, 0};
	entrance.DestinationWorld = core::Name("Far");
	entrance.Crosses = entrance.Bidirectional = true;
	const float scale = GENERATE(.25f, 1.f, 3.f);
	entrance.Scale = scale;
	scene::PortalSeam receiving = entrance;
	receiving.Scale = 1 / scale;
	receiving.First = receiving.First * scale;
	receiving.Second = receiving.Second * scale;
	receiving.Centre.Z = -.0002f;
	receiving.Normal = -core::Vector3::ZAxis;
	receiving.Destination = core::CFrame::LookAt({}, core::Vector3::ZAxis);
	receiving.DestinationWorld = core::Name("Near");
	const bool rotated = GENERATE(false, true);
	const auto placement = rotated
							   ? core::CFrame(core::Vector3{10, 4, 20}) * core::CFrame::Angles(.3f, .7f, -.4f)
							   : core::CFrame{};
	for (auto *seam : {&entrance, &receiving}) {
		seam->Centre = placement.PointToWorldSpace(seam->Centre);
		seam->Normal = placement.VectorToWorldSpace(seam->Normal);
		seam->First = placement.VectorToWorldSpace(seam->First);
		seam->Second = placement.VectorToWorldSpace(seam->Second);
		seam->Up = placement.UpVector();
		seam->Destination = placement * seam->Destination;
	}
	scene::CameraPortalView view;
	core::CFrame bodyFrame;
	const auto at = [&](float z) {
		return bodyFrame * placement * core::CFrame(core::Vector3{0, 0, z / scale});
	};
	REQUIRE(scene::StepCameraPortalView(view, "Near", at(1), {}) == scene::CameraPortalStep::Settled);
	REQUIRE(
		scene::StepCameraPortalView(view, "Near", at(-.0001f), {&entrance, 1}) ==
		scene::CameraPortalStep::Crossed
	);
	REQUIRE(
		scene::StepCameraPortalView(view, "Near", at(-.0001f), {&receiving, 1}) ==
		scene::CameraPortalStep::Settled
	);
	CHECK(
		scene::StepCameraPortalView(view, "Near", at(-.0003f), {&receiving, 1}) ==
		scene::CameraPortalStep::Settled
	);
	CHECK(view.World == "Far");
	SECTION("leave the uncertainty band before reversing") {
		REQUIRE(
			scene::StepCameraPortalView(view, "Near", at(-.02f), {&receiving, 1}) ==
			scene::CameraPortalStep::Settled
		);
	}
	SECTION("reverse slowly while still in the uncertainty band") {
		for (float z : {-.0001f, .0001f, .001f})
			CHECK(
				scene::StepCameraPortalView(view, "Near", at(z), {&receiving, 1}) ==
				scene::CameraPortalStep::Settled
			);
	}
	SECTION("body admission preserves the arrival side") {
		const auto point = view.ArrivalPoint;
		const auto normal = view.ArrivalNormal;
		bodyFrame = core::CFrame(core::Vector3{7, 3, -9}) * core::CFrame::Angles(.4f, -.6f, .2f);
		REQUIRE(scene::RebaseCameraPortalView(view, {bodyFrame, {}, 1}));
		CHECK((view.ArrivalPoint - point).Magnitude() == 0);
		CHECK((view.ArrivalNormal - normal).Magnitude() == 0);
		CHECK(view.ArrivedFrom == "Near");
	}
	CHECK(
		scene::StepCameraPortalView(view, "Near", at(.02f), {&receiving, 1}) ==
		scene::CameraPortalStep::Crossed
	);
	CHECK(view.World == "Near");
}

TEST_CASE("camera eye crosses an aperture independently of body admission", "[scene][camera-portal-view]") {
	scene::PortalSeam seam;
	seam.Normal = core::Vector3::ZAxis;
	seam.First = {2, 0, 0};
	seam.Second = {0, 2, 0};
	seam.Destination = core::CFrame(core::Vector3{10, 4, -20}) * core::CFrame::Angles(.2f, .7f, -.3f);
	seam.DestinationWorld = core::Name("Far");
	seam.Crosses = true;
	seam.Bidirectional = true;
	const float scale = GENERATE(.25f, 1.f, 3.f);
	{
		seam.Scale = scale;
		const auto through = scene::SeamMapping(seam);
		scene::CameraPortalView view;
		const core::CFrame before(core::Vector3{0, 0, 1});
		const core::CFrame after(core::Vector3{0, 0, -1});
		REQUIRE(scene::StepCameraPortalView(view, "Near", before, {}) == scene::CameraPortalStep::Settled);
		SECTION("eye crosses first") {
			REQUIRE(
				scene::StepCameraPortalView(view, "Near", core::CFrame{}, {&seam, 1}) ==
				scene::CameraPortalStep::Settled
			);
			REQUIRE(
				scene::StepCameraPortalView(view, "Near", after, {&seam, 1}) ==
				scene::CameraPortalStep::Crossed
			);
			CHECK(view.World == "Far");
			CHECK(
				(view.FromInput.Place(after).Position - through.Place(after).Position).Magnitude() < .0001f
			);
			REQUIRE(scene::RebaseCameraPortalView(view, through));
			CHECK(
				(view.FromInput.Place(through.Place(after)).Position - through.Place(after).Position)
					.Magnitude() < .0001f
			);
		}
		SECTION("body crosses first") {
			REQUIRE(scene::RebaseCameraPortalView(view, through));
			CHECK(view.World == "Near");
			CHECK(
				(view.FromInput.Place(through.Place(before)).Position - before.Position).Magnitude() < .0001f
			);
			REQUIRE(
				scene::StepCameraPortalView(view, "Far", through.Place(after), {&seam, 1}) ==
				scene::CameraPortalStep::Crossed
			);
			CHECK(view.World == "Far");
			CHECK(
				(view.FromInput.Place(through.Place(after)).Position - through.Place(after).Position)
					.Magnitude() < .0001f
			);
		}
		SECTION("outside aperture keeps source world") {
			REQUIRE(
				scene::StepCameraPortalView(
					view, "Near", core::CFrame(core::Vector3{10, 0, -1}), {&seam, 1}
				) == scene::CameraPortalStep::Settled
			);
			CHECK(view.World == "Near");
		}
	}
}
