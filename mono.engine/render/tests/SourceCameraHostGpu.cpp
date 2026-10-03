#include "ImageGraphTransform3DResident.hpp"
#include "RenderFixture.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
TEST_SUITE_ID("engine.render.source_camera_host_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.source_camera_capture")
TEST_CASE(
	"source camera asynchronous host publishes seven real surfaces and cancels exact generations",
	"[render][gpu][source-camera-host-gpu][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	imagegraph::Node node{"camera", "pc.3_d_camera", "", {}, {}};
	imagegraph::MeshValue3D mesh;
	auto &data = mesh.Data.emplace();
	data.LocalTransforms.emplace_back();
	data.Materials.emplace_back();
	imagegraph::MeshPart3D part;
	// Source identity rotation looks down +X, with screen right -Y and screen up -Z.
	for (const imagegraph::Vector3 position :
		 {imagegraph::Vector3{.5, 1, 1}, imagegraph::Vector3{.5, -3, 1}, imagegraph::Vector3{.5, 1, -3}})
		part.Vertices.push_back({position, {-1, 0, 0}, {0, 0}, {255, 0, 0, 255}});
	data.Parts.push_back(std::move(part));
	imagegraph::SceneValue3D scene;
	scene.Data.emplace().Objects.push_back({std::move(mesh)});
	imagegraph::MatrixValue projection{4, 4, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
	std::vector<imagegraph::AuthoredValue> controls = {
		{"scene", std::move(scene)},
		{"dimension", imagegraph::Vector2{4, 4}},
		{"dimension_unit", imagegraph::EnumValue{0}},
		{"postioning_mode", imagegraph::EnumValue{0}},
		{"projection", imagegraph::EnumValue{2}},
		{"projection_matrix", std::move(projection)},
		{"backface_culling", imagegraph::EnumValue{0}},
		{"ambient_light", imagegraph::Colour{255, 255, 255, 255}},
		{"shader", imagegraph::EnumValue{1}}
	};
	imagegraph::EvaluationRequest clock;
	imagegraph::HostNodeInvocation invocation{
		node,
		clock,
		controls,
		{},
		1 << 24,
		nullptr,
		imagegraph::SurfaceFormat::RGBA8Unorm,
		1,
		imagegraph::SourceCameraEvaluationPolicy{},
		0
	};
	const core::Name owner("camera-host-gpu"), name("camera-host-gpu/camera");
	imagegraph::HostNodeCapture output;
	output.Failure = "last good";
	std::string failure;
	bool pending = false;
	CHECK_FALSE(
		fixture.Render.CaptureSourceCamera3DAsync(invocation, owner, name, output, failure, &pending)
	);
	REQUIRE(pending);
	CHECK(output.Failure == "last good");
	CHECK_FALSE(
		fixture.Render.CaptureSourceCamera3DAsync(invocation, owner, name, output, failure, &pending)
	);
	REQUIRE(pending);
	using Access = render::test_support::TransformImage3DResidentTestAccess;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	bool ready = false;
	while (std::chrono::steady_clock::now() < deadline && !ready) {
		Access::Poll(fixture.Render);
		ready = fixture.Render.CaptureSourceCamera3DAsync(invocation, owner, name, output, failure, &pending);
		INFO(failure);
		REQUIRE((ready || pending));
		if (ready) break;
		const auto slots = Access::Slots(fixture.Render);
		if (std::any_of(slots.begin(), slots.end(), [](const auto &slot) {
				return slot.Phase == render::test_support::TransformImage3DQueuePhase::Queued;
			}))
			REQUIRE(Access::RecordAndSubmit(fixture.Render));
		SDL_Delay(1);
	}
	REQUIRE(ready);
	CHECK_FALSE(pending);
	REQUIRE(output.Images.size() == 7);
	for (const auto &image : output.Images) {
		CHECK(image.Data.Width == 4);
		CHECK(image.Data.Height == 4);
		CHECK(image.Data.Pixels.size() == 64);
		CHECK(imagegraph::ValidSurfaceLayout(image.Data, 4096, 1 << 24));
		CHECK(imagegraph::FiniteSurfaceSamples(image.Data));
	}
	CHECK(output.Images[1].Port == "diffuse");
	for (size_t pixel = 0; pixel < 16; ++pixel) {
		CHECK(output.Images[1].Data.Pixels[pixel * 4] == 255);
		CHECK(output.Images[1].Data.Pixels[pixel * 4 + 1] == 0);
		CHECK(output.Images[1].Data.Pixels[pixel * 4 + 2] == 0);
		CHECK(output.Images[1].Data.Pixels[pixel * 4 + 3] == 255);
	}
	const auto previous = output.Images[1].Data;
	clock.Tick = 1;
	CHECK_FALSE(
		fixture.Render.CaptureSourceCamera3DAsync(invocation, owner, name, output, failure, &pending)
	);
	REQUIRE(pending);
	CHECK(output.Images[1].Data == previous);
	fixture.Render.CancelComposerCapture(owner, name);
	clock.Tick = 0;
	CHECK_FALSE(
		fixture.Render.CaptureSourceCamera3DAsync(invocation, owner, name, output, failure, &pending)
	);
	REQUIRE(pending);
	fixture.Render.ForgetWorld(1, owner);
	CHECK_FALSE(
		fixture.Render.CaptureSourceCamera3DAsync(invocation, owner, name, output, failure, &pending)
	);
	REQUIRE(pending);
	fixture.Render.CancelComposerCapture(owner, name);
}
