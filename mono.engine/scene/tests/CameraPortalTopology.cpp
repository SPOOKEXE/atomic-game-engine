#include <engine/scene/CameraPortalTopology.hpp>
#include <engine/scene/CameraPortalView.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>

TEST_SUITE_ID("engine.scene.cameraportaltopology")

using namespace engine;

namespace {
	scene::CameraPortalTopology Topology() {
		scene::CameraPortalMouth mouth;
		mouth.Name = "Workspace/Door/Portal";
		mouth.DestinationWorld = "Far";
		mouth.First = {2, 0, 0};
		mouth.Second = {0, 3, 0};
		mouth.Up = {0, 1, 0};
		const auto destination =
			core::CFrame(core::Vector3{10, 4, -20}) * core::CFrame::Angles(.2f, .7f, -.3f);
		const auto rotation = destination.Rotation();
		mouth.Destination = {10, 4, -20, rotation.x, rotation.y, rotation.z, rotation.w};
		return {"Near", 7, {mouth}};
	}
}

TEST_CASE("owned camera topology drives the same scaled crossing", "[scene][camera-portal-topology]") {
	auto topology = Topology();
	topology.Mouths[0].Scale = GENERATE(.25f, 1.f, 3.f);
	topology.Mouths[0].Bidirectional = GENERATE(false, true);
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(scene::EncodeCameraPortalTopology(topology, bytes, error));
	scene::CameraPortalTopology decoded;
	REQUIRE(scene::DecodeCameraPortalTopology(bytes, decoded, error));
	CHECK(decoded == topology);
	std::vector<scene::PortalSeam> seams;
	REQUIRE(scene::ResolveCameraPortalTopology(decoded, seams, error));
	REQUIRE(seams.size() == 1);
	const auto &seam = seams.front();
	CHECK(seam.Pane == ecs::NULL_ENTITY);
	CHECK(seam.Far == ecs::NULL_ENTITY);
	CHECK(seam.Camera == ecs::NULL_ENTITY);
	CHECK(seam.Bidirectional == topology.Mouths[0].Bidirectional);
	const auto through = scene::SeamMapping(seam);
	scene::CameraPortalView eye;
	const core::CFrame before(core::Vector3{0, 0, 1});
	const core::CFrame after(core::Vector3{0, 0, -1});
	REQUIRE(scene::StepCameraPortalView(eye, decoded.World, before, {}) == scene::CameraPortalStep::Settled);
	REQUIRE(
		scene::StepCameraPortalView(eye, decoded.World, after, seams) == scene::CameraPortalStep::Crossed
	);
	CHECK(eye.World == "Far");
	CHECK((eye.FromInput.Place(after).Position - through.Place(after).Position).Magnitude() < .0001f);
	REQUIRE(scene::RebaseCameraPortalView(eye, through));
	CHECK(
		(eye.FromInput.Place(through.Place(after)).Position - through.Place(after).Position).Magnitude() <
		.0001f
	);
	REQUIRE(
		scene::StepCameraPortalView(eye, "Far", through.Place(after), {}) == scene::CameraPortalStep::Settled
	);
	scene::PortalSeam back;
	back.Centre = seam.Destination.Position;
	back.Normal = seam.Destination.LookVector();
	back.Up = seam.Destination.UpVector();
	back.First = through.Carry(seam.First);
	back.Second = through.Carry(seam.Second);
	back.Destination = core::CFrame::LookAt(seam.Centre, seam.Centre + seam.Normal, seam.Up);
	back.Scale = 1 / seam.Scale;
	back.Crosses = true;
	back.DestinationWorld = core::Name("Near");
	back.Pane = ecs::Entity{123};
	back.Camera = ecs::Entity{456};
	scene::CameraPortalMouth exit;
	REQUIRE(scene::CopyCameraPortalMouth("Return", back, exit, error));
	REQUIRE(scene::EncodeCameraPortalTopology({"Far", 8, {exit}}, bytes, error));
	REQUIRE(scene::DecodeCameraPortalTopology(bytes, decoded, error));
	REQUIRE(scene::ResolveCameraPortalTopology(decoded, seams, error));
	CHECK(seams[0].Pane == ecs::NULL_ENTITY);
	CHECK(seams[0].Camera == ecs::NULL_ENTITY);
	REQUIRE(
		scene::StepCameraPortalView(eye, "Far", through.Place(before), seams) ==
		scene::CameraPortalStep::Crossed
	);
	CHECK(eye.World == "Near");
	CHECK((eye.FromInput.Place(through.Place(before)).Position - before.Position).Magnitude() < .0001f);
}

TEST_CASE("copying a camera mouth refuses local and invalid seams", "[scene][camera-portal-topology]") {
	scene::PortalSeam seam;
	scene::CameraPortalMouth mouth = Topology().Mouths.front();
	const auto original = mouth;
	std::string error;
	CHECK_FALSE(scene::CopyCameraPortalMouth("Door", seam, mouth, error));
	CHECK(mouth == original);
	seam.Crosses = true;
	seam.DestinationWorld = core::Name("Far");
	CHECK_FALSE(scene::CopyCameraPortalMouth("Door", seam, mouth, error));
	CHECK(mouth == original);
}

TEST_CASE("camera topology accepts its maximum complete snapshot", "[scene][camera-portal-topology]") {
	auto topology = Topology();
	topology.World.assign(256, 'w');
	const auto mouth = topology.Mouths.front();
	topology.Mouths.assign(scene::MAX_CAMERA_PORTAL_SEAMS, mouth);
	for (size_t index = 0; index < topology.Mouths.size(); ++index) {
		auto &entry = topology.Mouths[index];
		entry.Name = std::to_string(index);
		entry.Name.resize(256, 'm');
		entry.DestinationWorld.assign(256, 'd');
	}
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(scene::EncodeCameraPortalTopology(topology, bytes, error));
	CHECK(bytes.size() <= scene::MAX_CAMERA_PORTAL_TOPOLOGY_BYTES);
	scene::CameraPortalTopology decoded;
	REQUIRE(scene::DecodeCameraPortalTopology(bytes, decoded, error));
	CHECK(decoded == topology);
}

TEST_CASE(
	"camera topology distinguishes empty publication from malformed data", "[scene][camera-portal-topology]"
) {
	std::string error;
	std::vector<std::byte> bytes;
	const scene::CameraPortalTopology empty{"Empty", 1, {}};
	REQUIRE(scene::EncodeCameraPortalTopology(empty, bytes, error));
	auto decoded = Topology();
	REQUIRE(scene::DecodeCameraPortalTopology(bytes, decoded, error));
	CHECK(decoded == empty);
	std::vector<scene::PortalSeam> seams(1);
	REQUIRE(scene::ResolveCameraPortalTopology(decoded, seams, error));
	CHECK(seams.empty());
	const auto valid = Topology();
	REQUIRE(scene::EncodeCameraPortalTopology(valid, bytes, error));
	for (size_t size = 0; size < bytes.size(); ++size) {
		CAPTURE(size);
		CHECK_FALSE(scene::DecodeCameraPortalTopology(std::span(bytes).first(size), decoded, error));
		CHECK(decoded == empty);
	}
	SECTION("trailing bytes") {
		bytes.push_back(std::byte{0});
	}
	SECTION("unsupported version") {
		bytes[4] = std::byte{2};
	}
	SECTION("reserved bits") {
		bytes[6] = std::byte{1};
	}
	SECTION("noncanonical policy") {
		bytes.back() = std::byte{2};
	}
	SECTION("oversized payload") {
		bytes.resize(scene::MAX_CAMERA_PORTAL_TOPOLOGY_BYTES + 1);
	}
	CHECK_FALSE(scene::DecodeCameraPortalTopology(bytes, decoded, error));
	CHECK(decoded == empty);
}

TEST_CASE(
	"camera topology rejects invalid geometry and identity transactionally", "[scene][camera-portal-topology]"
) {
	auto topology = Topology();
	auto &mouth = topology.Mouths.front();
	SECTION("empty world") {
		topology.World.clear();
	}
	SECTION("zero revision") {
		topology.Revision = 0;
	}
	SECTION("invalid name") {
		mouth.Name = std::string("a\0b", 3);
	}
	SECTION("oversized destination") {
		mouth.DestinationWorld.assign(257, 'x');
	}
	SECTION("same world") {
		mouth.DestinationWorld = topology.World;
	}
	SECTION("duplicate identity") {
		topology.Mouths.push_back(mouth);
	}
	SECTION("too many mouths") {
		topology.Mouths.resize(scene::MAX_CAMERA_PORTAL_SEAMS + 1);
	}
	SECTION("zero aperture") {
		mouth.First = {};
	}
	SECTION("underflow aperture") {
		mouth.First = {1e-30f, 0, 0};
	}
	SECTION("overflow aperture") {
		mouth.First = {1e30f, 0, 0};
	}
	SECTION("skew aperture") {
		mouth.Second = {1, 3, 0};
	}
	SECTION("normal length") {
		mouth.Normal = {0, 0, 2};
	}
	SECTION("normal alignment") {
		mouth.Normal = {1, 0, 0};
	}
	SECTION("up alignment") {
		mouth.Up = {0, 0, 1};
	}
	SECTION("nonfinite position") {
		mouth.Centre[0] = std::numeric_limits<float>::infinity();
	}
	SECTION("nonfinite rotation") {
		mouth.Destination[4] = std::numeric_limits<float>::quiet_NaN();
	}
	SECTION("nonunit rotation") {
		mouth.Destination[6] = 4;
	}
	SECTION("negative scale") {
		mouth.Scale = -1;
	}
	SECTION("zero scale") {
		mouth.Scale = 0;
	}
	std::string error;
	const std::vector<std::byte> original{std::byte{42}};
	auto bytes = original;
	CHECK_FALSE(scene::EncodeCameraPortalTopology(topology, bytes, error));
	CHECK(bytes == original);
	std::vector<scene::PortalSeam> seams(1);
	seams[0].Scale = 42;
	CHECK_FALSE(scene::ResolveCameraPortalTopology(topology, seams, error));
	REQUIRE(seams.size() == 1);
	CHECK(seams[0].Scale == 42);
}
