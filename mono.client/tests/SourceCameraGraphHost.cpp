#include "ImageGraphComposerAdapter.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("client.source_camera_graph_host")
TEST_DEPENDS("engine.imagegraph.source_camera_host")
TEST_CASE(
	"source camera typed scene reaches downstream client renderer host without ambient camera",
	"[client][source-camera-graph-host]"
) {
	using namespace engine::imagegraph;
	for (const auto type : {"pc.3_d_camera", "pc.3_d_camera_set"}) {
		DYNAMIC_SECTION(type) {
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"cube", "pc.3_d_mesh_cube", "", {}, {}},
				{"scene", "pc.3_d_scene", "", {}, {}},
				{"camera", type, "", {}, {}},
				{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
			};
			document.Nodes[1].DynamicInputs = {{"cube", ValueType::Mesh, std::nullopt}};
			document.Links = {
				{"cube", "mesh", "scene", "cube"},
				{"scene", "scene", "camera", "scene"},
				{"camera", "rendered", "invert", "image"}
			};
			document.Outputs = {{"surface", "invert", "image"}};
			Plan plan;
			Diagnostic diagnostic;
			const auto compiled = Compile(document, plan, diagnostic);
			INFO(diagnostic.Message);
			INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(compiled == Status::Ok);
			engine::render::Renderer renderer;
			std::vector<engine::core::Name> names;
			client::detail::ComposerProvider provider(
				renderer,
				engine::core::Name("camera.owner"),
				nullptr,
				engine::core::Name("camera.binding"),
				&names
			);
			EvaluationRequest request{.HostProvider = &provider};
			Image output{1, 1, {12, 34, 56, 255}};
			const auto evaluated = Evaluate(document, plan, "surface", request, output, diagnostic);
			INFO(diagnostic.Message);
			INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			CHECK(evaluated == Status::UnsupportedExecution);
			CHECK(diagnostic.NodeId == "camera");
			CHECK(diagnostic.Message.find("renderer device") != std::string::npos);
			CHECK_FALSE(provider.Pending);
			REQUIRE(names.size() == 1);
			CHECK(names[0].Text() == "camera.binding/camera");
		}
	}
}

TEST_CASE(
	"client camera provider replays bounded selected-row receipts before entering the idle device",
	"[client][source-camera-graph-host]"
) {
	using namespace engine::imagegraph;
	struct CompletedCapability final : HostNodeProvider {
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			Diagnostic diagnostic;
			uint64_t bytes = 0;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, output, bytes, diagnostic
				) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			output.Images.push_back({"rendered", Image{1, 1, {12, 34, 56, 255}}});
			return true;
		}
	} completed;
	Node camera{"camera", "pc.3_d_camera", "", {}, {}};
	SceneValue3D scene;
	scene.Data.emplace();
	std::vector<AuthoredValue> controls{{"scene", std::move(scene)}, {"position", Vector3{0, 0, 0}}};
	EvaluationRequest request{.Tick = 3};
	HostNodeInvocation invocation{
		camera,
		request,
		controls,
		{},
		1 << 20,
		nullptr,
		SurfaceFormat::RGBA8Unorm,
		1,
		SourceCameraEvaluationPolicy{},
		0
	};
	PendingHostObservations observations;
	HostNodeCapture receipt;
	std::string failure;
	observations.BeginAttempt();
	REQUIRE(observations.CaptureSequenced(invocation, completed, receipt, failure));
	controls[1].Data = Vector3{1, 0, 0};
	invocation.CameraRow = 1;
	REQUIRE(observations.CaptureSequenced(invocation, completed, receipt, failure));
	engine::render::Renderer renderer;
	std::vector<engine::core::Name> names;
	client::detail::ComposerProvider provider(
		renderer,
		engine::core::Name("camera.row.owner"),
		nullptr,
		engine::core::Name("camera.rows"),
		&names,
		&observations
	);
	observations.BeginAttempt();
	controls[1].Data = Vector3{0, 0, 0};
	invocation.CameraRow = 0;
	REQUIRE(provider.Capture(invocation, receipt, failure));
	controls[1].Data = Vector3{1, 0, 0};
	invocation.CameraRow = 1;
	REQUIRE(provider.Capture(invocation, receipt, failure));
	CHECK(names.empty());
	CHECK_FALSE(provider.Pending);
	CHECK(receipt.Images.front().Data.Pixels == std::vector<uint8_t>{12, 34, 56, 255});
	observations.BeginAttempt();
	controls[1].Data = Vector3{2, 0, 0};
	invocation.CameraRow = 0;
	CHECK_FALSE(provider.Capture(invocation, receipt, failure));
	CHECK(failure.find("changed") != std::string::npos);
	CHECK(names.empty());
	observations.Clear();
	observations.BeginAttempt();
	CHECK_FALSE(provider.Capture(invocation, receipt, failure));
	INFO(failure);
	CHECK(failure.find("renderer device") != std::string::npos);
	CHECK_FALSE(provider.Pending);
	REQUIRE(names.size() == 1);
	CHECK(names[0].Text() == "camera.rows/camera");
	// This is the same clear used by binding cancellation and source/owner replacement.
	observations.Clear();
	CHECK_FALSE(observations.PendingInput);
	CHECK(observations.Captures.empty());
}
