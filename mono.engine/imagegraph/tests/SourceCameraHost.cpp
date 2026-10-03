#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraph.source_camera_host")
TEST_DEPENDS("engine.imagegraph.host_capture")
namespace {
	using namespace engine::imagegraph;
	Document CameraGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Project.emplace();
		doc.Project->SurfaceWidth = 8;
		doc.Project->SurfaceHeight = 4;
		doc.Project->Shader3D = 1;
		doc.Nodes = {
			{"cube", "pc.3_d_mesh_cube", "", {}, {}},
			{"scene", "pc.3_d_scene", "", {}, {}},
			{"camera",
			 "pc.3_d_camera",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{1}}}},
			{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
		};
		doc.Nodes[1].DynamicInputs = {{"cube", ValueType::Mesh, std::nullopt}};
		doc.Links = {
			{"cube", "mesh", "scene", "cube"},
			{"scene", "scene", "camera", "scene"},
			{"camera", "rendered", "invert", "image"}
		};
		doc.Groups = {{"camera-group", "camera"}};
		for (auto &node : doc.Nodes)
			node.GroupId = "camera-group";
		doc.Outputs = {{"result", "invert", "image"}};
		return doc;
	}
	void RecordedOutputs(HostNodeCapture &receipt) {
		for (const std::string_view port :
			 {"rendered", "diffuse", "normal", "view_normal", "depth", "shadow", "ambient_occlusion"})
			receipt.Images.push_back({std::string(port), Image{1, 1, {12, 34, 56, 255}}});
	}
}
TEST_CASE(
	"source camera recordings feed a downstream image and reject changed source policy",
	"[imagegraph][source-camera-host]"
) {
	using namespace engine::imagegraph;
	auto document = CameraGraph();
	Plan plan;
	Diagnostic diagnostic;
	const auto compilationStatus0 = Compile(document, plan, diagnostic);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compilationStatus0 == Status::Ok);
	HostNodeCapture receipt;
	EvaluationRequest request;
	REQUIRE(PrepareHostCapture(document, plan, "camera", request, receipt, diagnostic) == Status::Ok);
	REQUIRE(receipt.CameraPolicy);
	CHECK(receipt.CameraPolicy->ProjectWidth == 8);
	CHECK(receipt.CameraPolicy->ProjectShader3D == 1);
	CHECK_FALSE(receipt.CameraPolicy->DimensionLinked);
	CHECK(receipt.CameraPolicy->InheritedSurfaceFormat == SurfaceFormat::RGBA8Unorm);
	RecordedOutputs(receipt);
	request.HostCaptures = {&receipt, 1};
	Image result;
	REQUIRE(Evaluate(document, plan, "result", request, result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{243, 221, 199, 255});
	document.Project->Shader3D = 0;
	CHECK(Evaluate(document, plan, "result", request, result, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Message.find("policy is stale") != std::string::npos);
	document.Project->Shader3D = 1;
	document.Groups[0].ColorDepth = 3;
	const auto compilationStatus1 = Compile(document, plan, diagnostic);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compilationStatus1 == Status::Ok);
	HostNodeCapture resolvedPolicy;
	REQUIRE(PrepareHostCapture(document, plan, "camera", request, resolvedPolicy, diagnostic) == Status::Ok);
	REQUIRE(resolvedPolicy.CameraPolicy);
	CHECK(resolvedPolicy.CameraPolicy->InheritedSurfaceFormat == SurfaceFormat::RGBA8Unorm);
	CHECK(resolvedPolicy.CameraPolicy == receipt.CameraPolicy);
	CHECK(Evaluate(document, plan, "result", request, result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{243, 221, 199, 255});

	document.Groups[0].ColorDepth = 4;
	const auto widerCompilation = Compile(document, plan, diagnostic);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(widerCompilation == Status::Ok);
	REQUIRE(PrepareHostCapture(document, plan, "camera", request, resolvedPolicy, diagnostic) == Status::Ok);
	REQUIRE(resolvedPolicy.CameraPolicy);
	CHECK(resolvedPolicy.CameraPolicy->InheritedSurfaceFormat == SurfaceFormat::RGBA16Float);
	CHECK(Evaluate(document, plan, "result", request, result, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Message.find("policy is stale") != std::string::npos);
	document.Groups[0].ColorDepth = 1;
	const auto compilationStatus2 = Compile(document, plan, diagnostic);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compilationStatus2 == Status::Ok);
	receipt.CameraPolicy->DimensionLinked = true;
	CHECK(Evaluate(document, plan, "result", request, result, diagnostic) == Status::InvalidValue);
	receipt.CameraPolicy.reset();
	CHECK(Evaluate(document, plan, "result", request, result, diagnostic) == Status::InvalidValue);
}
TEST_CASE(
	"borrowed camera controls preserve project units and shader inheritance without owning scene copies",
	"[imagegraph][source-camera-host]"
) {
	using namespace engine::imagegraph;
	Node node{"camera", "pc.3_d_camera", "", {}, {}};
	std::array<AuthoredValue, 3> controls{
		{{"dimension", Vector2{.5, .25}}, {"dimension_unit", EnumValue{1}}, {"shader", EnumValue{0}}}
	};
	SourceCameraEvaluationPolicy policy{64, 32, 1, 1, false};
	uint32_t width = 0, height = 0, shader = 0;
	Diagnostic diagnostic;
	REQUIRE(ResolveSourceCameraDimensions(node, policy, controls, width, height, diagnostic) == Status::Ok);
	CHECK(width == 32);
	CHECK(height == 8);
	REQUIRE(ResolveSourceCameraShader(policy, controls, shader, diagnostic) == Status::Ok);
	CHECK(shader == 1);
	policy.DimensionLinked = true;
	REQUIRE(ResolveSourceCameraDimensions(node, policy, controls, width, height, diagnostic) == Status::Ok);
	CHECK(width == 1);
	CHECK(height == 1);
	policy.ProjectWidth = 0;
	CHECK_FALSE(ValidSourceCameraEvaluationPolicy(policy));
}

TEST_CASE(
	"pending camera observations reject a changed inherited group format", "[imagegraph][source-camera-host]"
) {
	using namespace engine::imagegraph;
	struct RecordedProvider final : HostNodeProvider {
		uint32_t Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			++Calls;
			Diagnostic diagnostic;
			uint64_t bytes = 0;
			const auto status = PrepareResolvedHostCapture(
				invocation, invocation.MaximumOperationBytes, output, bytes, diagnostic
			);
			failure = diagnostic.Message;
			return status == Status::Ok;
		}
	} provider;
	Node node{"camera", "pc.3_d_camera", "", {}, {}};
	EvaluationRequest request;
	HostNodeInvocation invocation{
		node,
		request,
		{},
		{},
		1 << 20,
		nullptr,
		SurfaceFormat::RGBA8Unorm,
		1,
		SourceCameraEvaluationPolicy{},
		0
	};
	PendingHostObservations observations;
	HostNodeCapture output;
	std::string failure;
	REQUIRE(observations.Capture(invocation, provider, output, failure));
	CHECK(provider.Calls == 1);
	invocation.OutputFormat = SurfaceFormat::RGBA16Float;
	invocation.CameraPolicy->InheritedSurfaceFormat = SurfaceFormat::RGBA16Float;
	CHECK_FALSE(observations.Capture(invocation, provider, output, failure));
	CHECK(provider.Calls == 1);
	CHECK(output.CameraPolicy->InheritedSurfaceFormat == SurfaceFormat::RGBA8Unorm);
}

TEST_CASE(
	"source camera processor rows replay completed ordinals while the next row remains pending",
	"[imagegraph][source-camera-host]"
) {
	using namespace engine::imagegraph;
	struct DeviceCapability final : HostNodeProvider {
		std::vector<double> Calls;
		bool Ready = false;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			const auto position =
				std::find_if(invocation.Inputs.begin(), invocation.Inputs.end(), [](const auto &value) {
					return value.Port == "position";
				});
			REQUIRE(position != invocation.Inputs.end());
			const auto x = std::get<Vector3>(position->Data).X;
			Calls.push_back(x);
			if (x == 1 && !Ready) {
				failure = "source camera device capture is pending";
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
			RecordedOutputs(output);
			for (auto &image : output.Images)
				image.Data.Pixels[0] = x == 0 ? 10 : 20;
			return true;
		}
	} device;
	struct OrderedHost final : HostNodeProvider {
		PendingHostObservations Observations;
		HostNodeProvider &Device;
		explicit OrderedHost(HostNodeProvider &device) : Device(device) {}
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			return Observations.CaptureSequenced(invocation, Device, output, failure);
		}
	} host(device);
	auto document = CameraGraph();
	document.Nodes.push_back({"positions", "pc.array", "", {}, {}});
	document.Nodes.back().GroupId = "camera-group";
	document.Nodes.back().DynamicInputs = {
		{"input_0", ValueType::Vector3, Vector3{0, 0, 0}}, {"input_1", ValueType::Vector3, Vector3{1, 0, 0}}
	};
	document.Links.push_back({"positions", "array", "camera", "position"});
	document.Outputs = {{"rows", "camera", "rendered"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request{.Tick = 7, .Seed = 11, .HostProvider = &host};
	ImageArray rows;
	host.Observations.BeginAttempt();
	CHECK(EvaluateArray(document, plan, "rows", request, rows, diagnostic) == Status::UnsupportedExecution);
	CHECK(device.Calls == std::vector<double>{0, 1});
	REQUIRE(host.Observations.Captures.size() == 1);
	REQUIRE(host.Observations.PendingInput);
	device.Ready = true;
	host.Observations.BeginAttempt();
	const auto completed = EvaluateArray(document, plan, "rows", request, rows, diagnostic);
	INFO(diagnostic.Message);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(completed == Status::Ok);
	CHECK(device.Calls == std::vector<double>{0, 1, 1});
	REQUIRE(rows.Images.size() == 2);
	CHECK(rows.Images[0].Pixels[0] == 10);
	CHECK(rows.Images[1].Pixels[0] == 20);
	CHECK_FALSE(host.Observations.PendingInput);
	// Source changes are refused within this observation; owner cancellation clears it before replacement.
	document.Nodes.back().DynamicInputs[0].Default = Vector3{2, 0, 0};
	const auto compilationStatus3 = Compile(document, plan, diagnostic);
	INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compilationStatus3 == Status::Ok);
	host.Observations.BeginAttempt();
	CHECK(EvaluateArray(document, plan, "rows", request, rows, diagnostic) == Status::UnsupportedExecution);
	CHECK(device.Calls.size() == 3);
	host.Observations.Clear();
	host.Observations.BeginAttempt();
	REQUIRE(EvaluateArray(document, plan, "rows", request, rows, diagnostic) == Status::Ok);
	CHECK(device.Calls == std::vector<double>{0, 1, 1, 2, 1});
}

TEST_CASE(
	"recorded camera processor rows replay by explicit source ordinal including identical inputs",
	"[imagegraph][source-camera-host]"
) {
	using namespace engine::imagegraph;
	for (const bool identical : {false, true}) {
		DYNAMIC_SECTION("identical selected controls " << identical) {
			struct RecordingHost final : HostNodeProvider {
				std::vector<HostNodeCapture> Records;
				bool Capture(
					const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
				) override {
					REQUIRE(invocation.CameraRow);
					Diagnostic diagnostic;
					uint64_t bytes = 0;
					if (PrepareResolvedHostCapture(
							invocation, invocation.MaximumOperationBytes, output, bytes, diagnostic
						) != Status::Ok) {
						failure = diagnostic.Message;
						return false;
					}
					RecordedOutputs(output);
					for (auto &image : output.Images)
						image.Data.Pixels[0] = 10 + *invocation.CameraRow;
					Records.push_back(output);
					return true;
				}
			} host;
			auto document = CameraGraph();
			document.Nodes.push_back({"positions", "pc.array", "", {}, {}});
			document.Nodes.back().GroupId = "camera-group";
			document.Nodes.back().DynamicInputs = {
				{"input_0", ValueType::Vector3, Vector3{0, 0, 0}},
				{"input_1", ValueType::Vector3, Vector3{identical ? 0. : 1., 0, 0}}
			};
			document.Links.push_back({"positions", "array", "camera", "position"});
			document.Outputs = {{"rows", "camera", "rendered"}};
			Plan plan;
			Diagnostic diagnostic;
			const auto compilationStatus4 = Compile(document, plan, diagnostic);
			INFO("node=" << diagnostic.NodeId << " port=" << diagnostic.Port << " " << diagnostic.Message);
			REQUIRE(compilationStatus4 == Status::Ok);
			EvaluationRequest request{.Tick = 5, .HostProvider = &host};
			ImageArray live, replay;
			REQUIRE(EvaluateArray(document, plan, "rows", request, live, diagnostic) == Status::Ok);
			REQUIRE(host.Records.size() == 2);
			CHECK(host.Records[0].CameraRow == 0);
			CHECK(host.Records[1].CameraRow == 1);
			std::reverse(host.Records.begin(), host.Records.end());
			request.HostProvider = nullptr;
			request.HostCaptures = host.Records;
			REQUIRE(EvaluateArray(document, plan, "rows", request, replay, diagnostic) == Status::Ok);
			REQUIRE(replay.Images.size() == live.Images.size());
			for (size_t row = 0; row < live.Images.size(); ++row) {
				CHECK(replay.Images[row].Width == live.Images[row].Width);
				CHECK(replay.Images[row].Height == live.Images[row].Height);
				CHECK(replay.Images[row].Format == live.Images[row].Format);
				CHECK(replay.Images[row].Pixels == live.Images[row].Pixels);
			}
			REQUIRE(replay.Images.size() == 2);
			CHECK(replay.Images[0].Pixels[0] == 10);
			CHECK(replay.Images[1].Pixels[0] == 11);
			host.Records[0].CameraRow = 0;
			CHECK(EvaluateArray(document, plan, "rows", request, replay, diagnostic) == Status::DuplicateId);
			host.Records[0].CameraRow.reset();
			CHECK(EvaluateArray(document, plan, "rows", request, replay, diagnostic) == Status::InvalidValue);
			host.Records[0].CameraRow = Limits::MaximumArrayElements;
			CHECK(EvaluateArray(document, plan, "rows", request, replay, diagnostic) == Status::InvalidValue);
		}
	}
}

TEST_CASE(
	"public source camera helpers admit policy and counts before owning control views",
	"[imagegraph][source-camera-host]"
) {
	using namespace engine::imagegraph;
	Node camera{"camera", "pc.3_d_camera", "", {}, {}};
	SourceCameraEvaluationPolicy policy;
	std::vector<AuthoredValue> values(Limits::MaximumLinks + 1);
	Diagnostic diagnostic;
	uint32_t width = 7, height = 9, shader = 1;
	SurfaceFormat format = SurfaceFormat::RGBA16Float;
	CHECK(
		ResolveSourceCameraDimensions(camera, policy, values, width, height, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(
		ResolveSourceCameraSurfaceFormat(
			camera, policy, values, policy.InheritedSurfaceFormat, format, diagnostic
		) == Status::InvalidValue
	);
	CHECK(ResolveSourceCameraShader(policy, values, shader, diagnostic) == Status::InvalidValue);
	CHECK(width == 7);
	CHECK(height == 9);
	CHECK(shader == 1);
	CHECK(format == SurfaceFormat::RGBA16Float);
	policy.ProjectWidth = 0;
	values.clear();
	CHECK(
		ResolveSourceCameraDimensions(camera, policy, values, width, height, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(
		ResolveSourceCameraSurfaceFormat(
			camera, policy, values, policy.InheritedSurfaceFormat, format, diagnostic
		) == Status::InvalidValue
	);
	CHECK(ResolveSourceCameraShader(policy, values, shader, diagnostic) == Status::InvalidValue);
}
