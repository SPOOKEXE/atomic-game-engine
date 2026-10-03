#include "ImageGraphTransform3DResident.hpp"
#include "RenderFixture.hpp"

#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/render/SourceTransformImage3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>

TEST_SUITE_ID("engine.render.transformhost_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.sourcetransformimage3d")

TEST_CASE(
	"source Transform async receipt preserves both image channels mesh and submitted cancellation",
	"[render][gpu][transform-host-gpu][.]"
) {
	using namespace engine;
	using Access = render::test_support::TransformImage3DResidentTestAccess;
	using Phase = render::test_support::TransformImage3DQueuePhase;
	render::test::FixtureDevice device;
	device.Initialise();
	imagegraph::Node node{"transform", "pc.3_d_transform_image", "", {}, {}};
	imagegraph::EvaluationRequest clock{.Tick = 17};
	std::vector<imagegraph::AuthoredValue> controls{
		{"projection", imagegraph::EnumValue{1}},
		{"position", imagegraph::Vector3{}},
		{"scale", imagegraph::Vector3{1, 1, 1}},
		{"rotation", imagegraph::Quaternion{}},
		{"anchor", imagegraph::Vector3{}},
		{"fov", 45.},
		{"view_range", imagegraph::Vector2{.001, 10}},
		{"depth_range", imagegraph::Vector2{0, 1}},
		{"texture_tiling", imagegraph::Vector2{1, 1}}
	};
	imagegraph::Image front{3, 2, {}, 0}, back{1, 1, {0, 0, 255, 255}, 0};
	for (size_t i = 0; i < 6; ++i)
		front.Pixels.insert(front.Pixels.end(), {255, 0, 0, 255});
	const std::array<imagegraph::HostResolvedImage, 2> images{{{"surface", &front}, {"back_surface", &back}}};
	const core::Name owner("transform.gpu.owner"), name("transform.gpu.capture");
	const auto invocation = [&] {
		return imagegraph::HostNodeInvocation{node, clock, controls, images, 64ull * 1024 * 1024};
	};
	imagegraph::HostNodeCapture receipt;
	receipt.Failure = "retained";
	std::string failure;
	bool pending = false;
	CHECK_FALSE(
		device.Render.CaptureTransformImage3DAsync(invocation(), owner, name, receipt, failure, &pending)
	);
	INFO(failure);
	REQUIRE(pending);
	CHECK(receipt.Failure == "retained");
	const auto drive = [&](auto complete) {
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		size_t submissions = 0;
		while (std::chrono::steady_clock::now() < end) {
			Access::Poll(device.Render);
			if (complete()) return;
			const auto slots = Access::Slots(device.Render);
			if (std::any_of(slots.begin(), slots.end(), [](const auto &slot) {
					return slot.Phase == Phase::Queued;
				})) {
				REQUIRE(++submissions <= 16);
				REQUIRE(Access::RecordAndSubmit(device.Render));
			}
			SDL_Delay(1);
		}
		FAIL("Transform host did not complete within fixture deadline");
	};
	drive([&] {
		const bool ready =
			device.Render.CaptureTransformImage3DAsync(invocation(), owner, name, receipt, failure, &pending);
		INFO(failure);
		REQUIRE((ready || pending));
		return ready;
	});
	CHECK_FALSE(pending);
	CHECK(receipt.Tick == 17);
	REQUIRE(receipt.Images.size() == 2);
	REQUIRE(receipt.Outputs.size() == 1);
	const auto rendered = std::find_if(receipt.Images.begin(), receipt.Images.end(), [](const auto &entry) {
		return entry.Port == "rendered";
	});
	const auto depth = std::find_if(receipt.Images.begin(), receipt.Images.end(), [](const auto &entry) {
		return entry.Port == "depth";
	});
	REQUIRE(rendered != receipt.Images.end());
	REQUIRE(depth != receipt.Images.end());
	CHECK(rendered->Data.Pixels == front.Pixels);
	CHECK(rendered->Data.Format == imagegraph::SurfaceFormat::RGBA8Unorm);
	CHECK(depth->Data.Pixels.size() == 24);
	const auto *mesh = std::get_if<imagegraph::MeshValue3D>(&receipt.Outputs[0].Data);
	REQUIRE(mesh);
	REQUIRE(mesh->Data);
	CHECK(mesh->Data->Parts.size() == 2);
	CHECK(mesh->Data->Materials[1].Get().Surface->Width == 1);
	clock.Tick = 18;
	controls[3].Data = imagegraph::Quaternion{0, 1, 0, 0};
	CHECK_FALSE(
		device.Render.CaptureTransformImage3DAsync(invocation(), owner, name, receipt, failure, &pending)
	);
	REQUIRE(pending);
	drive([&] {
		const bool ready =
			device.Render.CaptureTransformImage3DAsync(invocation(), owner, name, receipt, failure, &pending);
		INFO(failure);
		REQUIRE((ready || pending));
		return ready;
	});
	std::vector<uint8_t> expected;
	for (size_t i = 0; i < 6; ++i)
		expected.insert(expected.end(), {0, 0, 255, 255});
	CHECK(receipt.Images[0].Data.Pixels == expected);
	clock.Tick = 19;
	CHECK_FALSE(
		device.Render.CaptureTransformImage3DAsync(invocation(), owner, name, receipt, failure, &pending)
	);
	REQUIRE(pending);
	REQUIRE(Access::RecordAndSubmit(device.Render));
	device.Render.CancelComposerCapture(owner, name);
	drive([&] {
		const auto slots = Access::Slots(device.Render);
		return std::none_of(slots.begin(), slots.end(), [&](const auto &slot) {
			return slot.Owner == owner && slot.Name == name && slot.Phase != Phase::Free;
		});
	});
	CHECK(receipt.Tick == 18);
	device.Render.ForgetWorld(0, owner);
	CHECK(device.Render.SourceOutputStatus(owner, name, 1) == render::SourceTextureStatus::Absent);
}
