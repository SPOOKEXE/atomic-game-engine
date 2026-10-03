#include "ImageGraphPreviewProvider.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("studio.imagegraph.preview_provider")
TEST_DEPENDS("studio.imagegraph.host")
namespace {
	using namespace engine::imagegraph;
	Document Cameras(std::string type) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Project.emplace();
		doc.Project->SurfaceWidth = 8;
		doc.Project->SurfaceHeight = 4;
		doc.Project->Shader3D = 1;
		doc.Nodes = {
			{"cube", "pc.3_d_mesh_cube", {}, {}, {}},
			{"scene", "pc.3_d_scene", {}, {}, {}},
			{"camera",
			 std::move(type),
			 {},
			 {},
			 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{1}}}},
			{"positions", "pc.array", {}, {}, {}}
		};
		doc.Nodes[1].DynamicInputs = {{"cube", ValueType::Mesh, std::nullopt}};
		doc.Nodes[3].DynamicInputs = {
			{"input_0", ValueType::Vector3, Vector3{0, 0, 0}},
			{"input_1", ValueType::Vector3, Vector3{0, 0, 0}}
		};
		doc.Groups = {{"camera-group", "camera"}};
		for (auto &node : doc.Nodes)
			node.GroupId = "camera-group";
		doc.Links = {
			{"cube", "mesh", "scene", "cube"},
			{"scene", "scene", "camera", "scene"},
			{"positions", "array", "camera", "position"}
		};
		doc.Outputs = {{"rows", "camera", "rendered"}};
		return doc;
	}
	// An explicit delayed capability exercises the real source evaluator and
	// composite dispatcher. This does not stand in for hardware rendering.
	struct CameraCapability final : HostNodeProvider {
		bool Ready = false;
		std::vector<uint32_t> Calls;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			REQUIRE(invocation.CameraRow);
			Calls.push_back(*invocation.CameraRow);
			if (*invocation.CameraRow == 1 && !Ready) {
				failure = "Camera observation is pending";
				return false;
			}
			uint64_t bytes = 0;
			Diagnostic error;
			HostNodeCapture candidate;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, candidate, bytes, error
				) != Status::Ok) {
				failure = error.Message;
				return false;
			}
			for (const std::string_view port :
				 {"rendered", "diffuse", "normal", "view_normal", "depth", "shadow", "ambient_occlusion"}) {
				Image image{1, 1, {static_cast<uint8_t>(10 + *invocation.CameraRow), 34, 56, 255}};
				image.Hash = SurfaceHash(image);
				candidate.Images.push_back({std::string(port), std::move(image)});
			}
			output = std::move(candidate);
			return true;
		}
	};
}
TEST_CASE(
	"Studio preview retries identical camera processor rows without rerunning completed ordinals",
	"[studio][camera_preview]"
) {
	using namespace studio::detail;
	for (const std::string type : {"pc.3_d_camera", "pc.3_d_camera_set"}) {
		INFO(type);
		auto doc = Cameras(type);
		Plan plan;
		Diagnostic error;
		const auto compilationStatus0 = Compile(doc, plan, error);
		INFO("node=" << error.NodeId << " port=" << error.Port << " " << error.Message);
		REQUIRE(compilationStatus0 == Status::Ok);
		CameraCapability capability;
		ImageGraphHost composite;
		composite.Composer = &capability;
		ImageGraphPreviewObservations observations;
		EvaluationRequest request{.Tick = 7, .Seed = 11};
		ImageGraphPreviewProvider provider(composite, observations, request);
		request.HostProvider = &provider;
		ImageArray output;
		for (unsigned attempt = 0; attempt < 3; ++attempt) {
			observations.Begin({7});
			CHECK(EvaluateArray(doc, plan, "rows", request, output, error) == Status::UnsupportedExecution);
			REQUIRE(observations.Receipts.Captures.size() == 1);
			CHECK(observations.Receipts.Captures[0].CameraRow == 0);
			CHECK(capability.Calls.size() == attempt + 2);
			CHECK(output.Images.empty());
		}
		CHECK(capability.Calls == std::vector<uint32_t>{0, 1, 1, 1});
		capability.Ready = true;
		observations.Begin({7});
		const auto completed = EvaluateArray(doc, plan, "rows", request, output, error);
		INFO(error.Message);
		INFO("node=" << error.NodeId << " port=" << error.Port);
		REQUIRE(completed == Status::Ok);
		REQUIRE(output.Images.size() == 2);
		CHECK(output.Images[0].Pixels[0] == 10);
		CHECK(output.Images[1].Pixels[0] == 11);
		CHECK(capability.Calls == std::vector<uint32_t>{0, 1, 1, 1, 1});
		const auto calls = capability.Calls.size();
		observations.Begin({7});
		REQUIRE(EvaluateArray(doc, plan, "rows", request, output, error) == Status::Ok);
		CHECK(capability.Calls.size() == calls);
		doc.Nodes[3].DynamicInputs[0].Default = Vector3{2, 0, 0};
		const auto compilationStatus1 = Compile(doc, plan, error);
		INFO("node=" << error.NodeId << " port=" << error.Port << " " << error.Message);
		REQUIRE(compilationStatus1 == Status::Ok);
		observations.Begin({7});
		CHECK(EvaluateArray(doc, plan, "rows", request, output, error) == Status::UnsupportedExecution);
		CHECK(capability.Calls.size() == calls);
		REQUIRE(output.Images.size() == 2);
		CHECK(output.Images[0].Pixels[0] == 10);
		CHECK(output.Images[1].Pixels[0] == 11);
		observations.Clear();
		CHECK_FALSE(observations.Frame);
		CHECK(observations.Receipts.Captures.empty());
		observations.Begin({7});
		REQUIRE(EvaluateArray(doc, plan, "rows", request, output, error) == Status::Ok);
		CHECK(capability.Calls.size() == calls + 2);
		request.Tick = 8;
		observations.Begin({8});
		REQUIRE(EvaluateArray(doc, plan, "rows", request, output, error) == Status::Ok);
		CHECK(capability.Calls.size() == calls + 4);
		// Other caller scopes borrow the composite directly; the preview journal
		// is not installed globally or applied again to export's own wrapper.
		request.Tick = 9;
		request.HostProvider = &composite;
		REQUIRE(EvaluateArray(doc, plan, "rows", request, output, error) == Status::Ok);
		CHECK(capability.Calls.size() == calls + 6);
		CHECK(observations.Frame == std::optional(FrameTime{8}));
		CHECK(observations.Receipts.Captures.size() == 2);
	}
}
TEST_CASE("Studio preview budget refusal preserves completed camera receipts", "[studio][camera_preview]") {
	using namespace studio::detail;
	Document doc = Cameras("pc.3_d_camera");
	Plan plan;
	Diagnostic error;
	const auto compilationStatus2 = Compile(doc, plan, error);
	INFO("node=" << error.NodeId << " port=" << error.Port << " " << error.Message);
	REQUIRE(compilationStatus2 == Status::Ok);
	CameraCapability capability;
	ImageGraphHost composite;
	composite.Composer = &capability;
	ImageGraphPreviewObservations observations;
	EvaluationRequest request;
	ImageGraphPreviewProvider provider(composite, observations, request);
	request.HostProvider = &provider;
	ImageArray output;
	observations.Begin({0});
	CHECK(EvaluateArray(doc, plan, "rows", request, output, error) == Status::UnsupportedExecution);
	REQUIRE(observations.Receipts.Captures.size() == 1);
	const auto held = observations.Receipts.RetainedPayloadBytes();
	const auto calls = capability.Calls;
	observations.Begin({0});
	StatefulEvaluationResult bounded;
	CHECK(EvaluateStateful(doc, plan, "rows", request, bounded, error, 1) != Status::Ok);
	CHECK(capability.Calls == calls);
	CHECK(observations.Receipts.RetainedPayloadBytes() == held);
	REQUIRE(observations.Receipts.Captures.size() == 1);
	CHECK(observations.Receipts.Captures[0].CameraRow == 0);
}
