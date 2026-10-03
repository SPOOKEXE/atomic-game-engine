#include "ImageGraphCameraRoute.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraph.camera_route")
TEST_DEPENDS("engine.imagegraph.source_camera_host")
TEST_DEPENDS("engine.scene.imagegraphbinding")
namespace {
	using namespace engine::imagegraph;
	Document CameraGraph(bool rows) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"cube", "pc.3_d_mesh_cube", "", {}, {}},
			{"scene", "pc.3_d_scene", "", {}, {}},
			{"camera",
			 "pc.3_d_camera",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
		};
		document.Nodes[1].DynamicInputs = {{"object_0", ValueType::Mesh, std::nullopt}};
		document.Links = {{"cube", "mesh", "scene", "object_0"}, {"scene", "scene", "camera", "scene"}};
		document.Outputs = {{"terminal", "camera", "rendered"}};
		if (rows) {
			document.Nodes.push_back({"positions", "pc.array", "", {}, {}});
			document.Nodes.back().DynamicInputs = {
				{"input_0", ValueType::Vector3, Vector3{0, 0, 0}},
				{"input_1", ValueType::Vector3, Vector3{1, 0, 0}}
			};
			document.Links.push_back({"positions", "array", "camera", "position"});
		}
		return document;
	}
}
TEST_CASE("Camera fast routing proves scene processing is singleton", "[client][camera-route]") {
	auto document = CameraGraph(false);
	REQUIRE(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
	document.Nodes[2].Type = "pc.3_d_camera_set";
	CHECK(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
	document = CameraGraph(true);
	CHECK_FALSE(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
	// The scene gatherer runs after its own input processor. A processed cube is not scalar proof.
	document.Links.back() = {"positions", "array", "cube", "position"};
	CHECK_FALSE(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
	document = CameraGraph(false);
	document.Keyframes = {{"cube", "position", 0, Vector3{}, "step"}};
	CHECK_FALSE(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
	document = CameraGraph(false);
	ArrayValue values;
	values.ElementType = ValueType::Vector3;
	values.Elements = {Vector3{}, Vector3{1, 0, 0}};
	document.Nodes[2].Values.push_back({"position", std::move(values)});
	CHECK_FALSE(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
}
TEST_CASE("Terminal camera rows retain completed observations during async retry", "[client][camera-route]") {
	struct DeviceObservation final : HostNodeProvider {
		std::vector<uint32_t> Calls;
		bool Delayed = false;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			REQUIRE(invocation.CameraRow);
			Calls.push_back(*invocation.CameraRow);
			if (*invocation.CameraRow == 1 && !Delayed) {
				Delayed = true;
				failure = "explicit camera observation is pending";
				return false;
			}
			Diagnostic diagnostic;
			uint64_t bytes = 0;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, output, bytes, diagnostic
				) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			for (const auto port :
				 {"rendered", "diffuse", "normal", "view_normal", "depth", "shadow", "ambient_occlusion"}) {
				output.Images.push_back(
					{port, Image{1, 1, {uint8_t(10 + *invocation.CameraRow), 34, 56, 255}}}
				);
			}
			return true;
		}
	} device;
	struct Sequenced final : HostNodeProvider {
		PendingHostObservations Observations;
		HostNodeProvider &Device;
		explicit Sequenced(HostNodeProvider &device) : Device(device) {}
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			return Observations.CaptureSequenced(invocation, Device, output, failure);
		}
	} host(device);
	const auto document = CameraGraph(true);
	REQUIRE_FALSE(client::detail::SourceCameraSingletonCone(document, document.Nodes[2]));
	Plan plan;
	Diagnostic diagnostic;
	const auto compilationStatus0 = Compile(document, plan, diagnostic);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compilationStatus0 == Status::Ok);
	EvaluationRequest request{.Tick = 3, .HostProvider = &host};
	StatefulEvaluationResult value;
	host.Observations.BeginAttempt();
	CHECK(
		EvaluateStateful(document, plan, "terminal", request, value, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(device.Calls == std::vector<uint32_t>{0, 1});
	host.Observations.BeginAttempt();
	const auto status = EvaluateStateful(document, plan, "terminal", request, value, diagnostic);
	INFO(diagnostic.Message);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	CHECK(device.Calls == std::vector<uint32_t>{0, 1, 1});
	const auto *images = std::get_if<ImageArray>(&value.Output);
	REQUIRE(images);
	REQUIRE(images->Images.size() == 2);
	CHECK(images->Images[0].Pixels[0] == 10);
	CHECK(images->Images[1].Pixels[0] == 11);
}
TEST_CASE(
	"Actual terminal array binding enters core camera host before device admission", "[client][camera-route]"
) {
	struct GraphFile {
		std::filesystem::path Assets = std::filesystem::temp_directory_path() / "atomic-camera-terminal-rows";
		engine::core::Name Graph{"camera-rows"};
		GraphFile() {
			std::filesystem::remove_all(Assets);
			std::filesystem::create_directories(Assets / "imagegraphs");
			std::ofstream file(client::ImageGraphDocumentPath(Assets, Graph));
			file << Write(CameraGraph(true));
		}
		~GraphFile() {
			std::error_code error;
			std::filesystem::remove_all(Assets, error);
		}
	} file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("camera-terminal-route");
	const auto entity = store.Create();
	engine::scene::ImageGraphBinding binding;
	binding.Graph = file.Graph;
	binding.Output = engine::core::Name("terminal");
	binding.Texture = engine::core::Name("camera-terminal-texture");
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, binding));
	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	CHECK(runtime.Refresh(store, renderer, engine::core::Name("camera-terminal-owner"), file.Assets) == 0);
	INFO(runtime.LastError());
	CHECK(runtime.LastError().find("renderer device") != std::string::npos);
	runtime.Clear(renderer);
}
