#include "../src/ImageGraphTransform3DResident.hpp"

#include <engine/render/SourceCamera3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <limits>
#include <string>

TEST_SUITE_ID("engine.render.sourcecamera3d")

namespace {
	engine::render::imagegraph::SourceCamera3DRequest Request() {
		engine::render::imagegraph::SourceCamera3DRequest request;
		request.Width = request.Height = 4;
		const glm::mat4 identity(1);
		std::copy_n(glm::value_ptr(identity), 16, request.View.begin());
		std::copy_n(glm::value_ptr(identity), 16, request.Projection.begin());
		return request;
	}
}
TEST_CASE(
	"source camera admission validates finite controls and bounded typed scene payloads",
	"[render][imagegraph]"
) {
	using namespace engine::render::imagegraph;
	auto request = Request();
	CHECK(ValidateSourceCamera3D(request) == SourceCamera3DStatus::Ok);
	request.Projection[0] = 0;
	CHECK(ValidateSourceCamera3D(request) == SourceCamera3DStatus::InvalidControl);
	request = Request();
	request.AoRadius = std::numeric_limits<float>::infinity();
	CHECK(ValidateSourceCamera3D(request) == SourceCamera3DStatus::InvalidControl);
	request = Request();
	request.Width = request.Height = 4096;
	CHECK(ValidateSourceCamera3D(request) == SourceCamera3DStatus::OutputLimit);
	request = Request();
	request.Scene.Data.emplace().Transform.Rotation = {0, 0, 0, 0};
	CHECK(ValidateSourceCamera3D(request) == SourceCamera3DStatus::Ok);
	request.Scene.Data->Transform.Rotation.X = std::numeric_limits<double>::quiet_NaN();
	CHECK(ValidateSourceCamera3D(request) == SourceCamera3DStatus::InvalidScene);
}
TEST_CASE(
	"equal source camera controls multiplex seven named outputs and cancel only one binding",
	"[render][imagegraph]"
) {
	using namespace engine;
	render::Renderer renderer;
	const core::Name owner("source-camera-owner");
	for (uint32_t output = 0; output < 7; ++output) {
		auto request = Request();
		request.Output = static_cast<render::imagegraph::SourceCamera3DOutput>(output);
		const core::Name name("source-camera-output-" + std::to_string(output));
		const auto status = renderer.QueueSourceCamera3D({owner, name, output + 1, std::move(request)});
		CHECK(
			status == (output == 0 ? render::imagegraph::TransformImage3DQueueResult::Queued
								   : render::imagegraph::TransformImage3DQueueResult::Replaced)
		);
	}
	auto slots = render::test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	size_t occupied = 0;
	for (const auto &slot : slots)
		if (slot.Phase != render::test_support::TransformImage3DQueuePhase::Free) {
			++occupied;
			CHECK(slot.CameraOutputs == 7);
		}
	CHECK(occupied == 1);
	REQUIRE(renderer.CancelTransformImage3D(owner, core::Name("source-camera-output-3"), 4));
	slots = render::test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	for (const auto &slot : slots)
		if (slot.Phase != render::test_support::TransformImage3DQueuePhase::Free)
			CHECK(slot.CameraOutputs == 6);
	renderer.DropTransformImage3DOwner(owner);
	CHECK(render::test_support::TransformImage3DResidentTestAccess::SourceBytes(renderer) == 0);
}
TEST_CASE("a changed source camera control retains separate fenced work", "[render][imagegraph]") {
	using namespace engine;
	render::Renderer renderer;
	const core::Name owner("source-camera-separate");
	auto first = Request();
	auto second = first;
	second.AoStrength = 2;
	REQUIRE(
		renderer.QueueSourceCamera3D({owner, core::Name("source-camera-first"), 1, std::move(first)}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	REQUIRE(
		renderer.QueueSourceCamera3D({owner, core::Name("source-camera-second"), 2, std::move(second)}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	const auto slots = render::test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	CHECK(std::count_if(slots.begin(), slots.end(), [](const auto &slot) {
			  return slot.CameraOutputs == 1;
		  }) == 2);
	renderer.DropTransformImage3DOwner(owner);
}

TEST_CASE("rebinding one camera output to a plane preserves sibling aliases", "[render][imagegraph]") {
	using namespace engine;
	render::Renderer renderer;
	const core::Name owner("camera-plane-owner"), first("camera-plane-first"), second("camera-plane-second");
	auto camera = Request();
	REQUIRE(
		renderer.QueueSourceCamera3D({owner, first, 1, camera}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	camera.Output = render::imagegraph::SourceCamera3DOutput::Normal;
	REQUIRE(
		renderer.QueueSourceCamera3D({owner, second, 2, camera}) ==
		render::imagegraph::TransformImage3DQueueResult::Replaced
	);
	render::imagegraph::TransformImage3DRequest plane;
	plane.Front.Width = plane.Front.Height = 1;
	plane.Front.Pixels.assign(4, std::byte{255});
	REQUIRE(
		renderer.QueueTransformImage3D(
			{owner, first, 3, render::imagegraph::TransformImage3DOutput::Rendered, std::move(plane)}
		) == render::imagegraph::TransformImage3DQueueResult::Queued
	);
	const auto slots = render::test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	size_t active = 0, cameraOutputs = 0;
	for (const auto &slot : slots) {
		active += slot.Phase != render::test_support::TransformImage3DQueuePhase::Free;
		cameraOutputs += slot.CameraOutputs;
	}
	CHECK(active == 2);
	CHECK(cameraOutputs == 1);
	CHECK_FALSE(renderer.CancelTransformImage3D(owner, first, 1));
	CHECK(renderer.CancelTransformImage3D(owner, second, 2));
}

TEST_CASE(
	"source camera validates particle identity and finite separate metadata",
	"[render][imagegraph][particle3d]"
) {
	using namespace engine;
	auto request = Request();
	imagegraph::MeshValue3D particleMesh;
	auto &mesh = particleMesh.Data.emplace();
	mesh.LocalTransforms.emplace_back();
	mesh.Instanced = mesh.ParticleInstanced = true;
	request.Scene.Data.emplace().Objects.emplace_back(imagegraph::SceneObject3D{std::move(particleMesh)});
	CHECK(
		render::imagegraph::ValidateSourceCamera3D(request) == render::imagegraph::SourceCamera3DStatus::Ok
	);
	auto &owned = *std::get<imagegraph::MeshValue3D>(request.Scene.Data->Objects[0].Data).Data;
	owned.ParticleRecords.emplace_back();
	CHECK(
		render::imagegraph::ValidateSourceCamera3D(request) ==
		render::imagegraph::SourceCamera3DStatus::InvalidScene
	);
	owned.Instances.emplace_back();
	CHECK(
		render::imagegraph::ValidateSourceCamera3D(request) == render::imagegraph::SourceCamera3DStatus::Ok
	);
	owned.ParticleRecords[0].Velocity[2] = std::numeric_limits<float>::infinity();
	CHECK(
		render::imagegraph::ValidateSourceCamera3D(request) ==
		render::imagegraph::SourceCamera3DStatus::InvalidScene
	);
	owned.ParticleRecords[0].Velocity[2] = 0;
	owned.ParticleInstanced = false;
	CHECK(
		render::imagegraph::ValidateSourceCamera3D(request) ==
		render::imagegraph::SourceCamera3DStatus::InvalidScene
	);
	owned.ParticleInstanced = true;
	owned.Instanced = false;
	CHECK(
		render::imagegraph::ValidateSourceCamera3D(request) ==
		render::imagegraph::SourceCamera3DStatus::InvalidScene
	);
}
