#include "ImageGraphPreviewResult.hpp"

#include "ImageGraphPreviewProvider.hpp"
#include "ImageGraphPreviewSequence.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("studio.imagegraph.preview_result")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.feedback_host")
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
	"RefreshPreview result publishes terminal camera rows without rerunning completed ordinals",
	"[studio][camera_preview][preview-result]"
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
		CapturedFeedbackHost feedback;
		studio::ImageGraphPreviewValue output;
		for (unsigned attempt = 0; attempt < 3; ++attempt) {
			observations.Begin({7});
			CHECK(
				PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error) ==
				Status::UnsupportedExecution
			);
			REQUIRE(observations.Receipts.Captures.size() == 1);
			CHECK(observations.Receipts.Captures[0].CameraRow == 0);
			CHECK(capability.Calls.size() == attempt + 2);
			CHECK(std::get<Image>(output).Pixels.empty());
		}
		CHECK(capability.Calls == std::vector<uint32_t>{0, 1, 1, 1});
		capability.Ready = true;
		observations.Begin({7});
		const auto completed =
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error);
		INFO(error.Message);
		INFO("node=" << error.NodeId << " port=" << error.Port);
		REQUIRE(completed == Status::Ok);
		REQUIRE(std::get<ImageArray>(output).Images.size() == 2);
		CHECK(std::get<ImageArray>(output).Images[0].Pixels[0] == 10);
		CHECK(std::get<ImageArray>(output).Images[1].Pixels[0] == 11);
		CHECK(capability.Calls == std::vector<uint32_t>{0, 1, 1, 1, 1});
		const auto calls = capability.Calls.size();
		observations.Begin({7});
		REQUIRE(
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error) ==
			Status::Ok
		);
		CHECK(capability.Calls.size() == calls);
		ImageGraphPreviewSequence sequence;
		ImageGraphPreviewSequence::Key key{{engine::core::Name("preview"), "rows", 1, 1, 0}, {7}, 0};
		REQUIRE(sequence.Admit(std::get<ImageArray>(output), key, Limits::MaximumEvaluationBytes, error));
		sequence.Publish(std::move(std::get<ImageArray>(output)), std::move(key));
		CHECK(sequence.Selected().Image->Pixels[0] == 10);
		sequence.Indices[0] = 3;
		CHECK(sequence.Selected().Image->Pixels[0] == 11);
		CHECK(capability.Calls.size() == calls);
		// Preserve a prior result across a refused new authored generation.
		output = sequence.Data;
		doc.Nodes[3].DynamicInputs[0].Default = Vector3{2, 0, 0};
		const auto compilationStatus1 = Compile(doc, plan, error);
		INFO("node=" << error.NodeId << " port=" << error.Port << " " << error.Message);
		REQUIRE(compilationStatus1 == Status::Ok);
		observations.Begin({7});
		CHECK(
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error) ==
			Status::UnsupportedExecution
		);
		CHECK(capability.Calls.size() == calls);
		REQUIRE(std::get<ImageArray>(output).Images.size() == 2);
		CHECK(std::get<ImageArray>(output).Images[0].Pixels[0] == 10);
		CHECK(std::get<ImageArray>(output).Images[1].Pixels[0] == 11);
		observations.Clear();
		CHECK_FALSE(observations.Frame);
		CHECK(observations.Receipts.Captures.empty());
		observations.Begin({7});
		REQUIRE(
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error) ==
			Status::Ok
		);
		CHECK(capability.Calls.size() == calls + 2);
		request.Tick = 8;
		observations.Begin({8});
		REQUIRE(
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error) ==
			Status::Ok
		);
		CHECK(capability.Calls.size() == calls + 4);
		// Other caller scopes borrow the composite directly; the preview journal
		// is not installed globally or applied again to export's own wrapper.
		request.Tick = 9;
		request.HostProvider = &composite;
		REQUIRE(
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, output, error) ==
			Status::Ok
		);
		CHECK(capability.Calls.size() == calls + 6);
		CHECK(observations.Frame == std::optional(FrameTime{8}));
		CHECK(observations.Receipts.Captures.size() == 2);
	}
}
namespace {
	Image Pixel(uint8_t value) {
		Image image{1, 1, {value, 34, 56, 255}};
		image.Hash = SurfaceHash(image);
		return image;
	}
	studio::detail::ImageGraphPreviewSequence::Key PreviewKey(uint64_t revision = 1) {
		return {{engine::core::Name("preview-world"), "rows", revision, 1, 0}, {7}, 0};
	}
}
TEST_CASE(
	"RefreshPreview stateful union publishes sequences without rerunning capabilities",
	"[studio][preview-result]"
) {
	using namespace studio::detail;
	Document doc;
	doc.FormatVersion = 9;
	doc.Project.emplace();
	doc.Project->SurfaceWidth = doc.Project->SurfaceHeight = 1;
	doc.Nodes = {
		{"prior", "image.captured", {}, {}, {{"source_id", std::string("feedback:out")}}},
		{"invert", "image.invert", {}, {}, {{"include_alpha", false}}},
		{"argument",
		 "pc.argument",
		 {},
		 {},
		 {{"tag", std::string("width")}, {"type", EnumValue{1}}, {"default_value", int64_t{1}}}},
		{"solid",
		 "pc.solid",
		 {},
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{12, 34, 56, 255}}}},
		{"array", "value.array", {}, {}, {}}
	};
	doc.Nodes[4].DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	doc.Links = {
		{"prior", "image", "invert", "image"},
		{"argument", "value", "solid", "dimension"},
		{"solid", "surface_out", "array", "first"},
		{"invert", "image", "array", "second"}
	};
	doc.Outputs = {{"out", "invert", "image"}, {"rows", "array", "array"}};
	struct CountingArgument final : HostNodeProvider {
		SourceArgumentHost Arguments;
		size_t Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			++Calls;
			return Arguments.Capture(invocation, output, failure);
		}
	} provider;
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	INFO("node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost feedback;
	studio::ImageGraphPreviewValue preview;
	for (uint64_t tick = 0; tick < 2; ++tick) {
		EvaluationRequest request{.Tick = tick};
		request.HostProvider = &provider;
		const auto status =
			PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, preview, error);
		INFO(error.Message);
		INFO("node=" << error.NodeId << " port=" << error.Port);
		REQUIRE(status == Status::Ok);
		REQUIRE(feedback.Active());
		REQUIRE(feedback.Value("rows"));
		const auto &array = std::get<ImageArray>(preview);
		REQUIRE(array.Images.size() == 2);
		CHECK(array.Images[0].Pixels[0] == 12);
		CHECK(array.Images[1].Pixels[0] == (tick ? 0 : 255));
		CHECK(provider.Calls == tick + 1);
	}
	const auto held = feedback.RetainedBytes();
	const auto before = std::get<ImageArray>(preview).Images;
	EvaluationRequest request{.Tick = 1};
	request.HostProvider = &provider;
	CHECK(
		PrepareImageGraphPreviewResult(doc, plan, "rows", request, feedback, 1, 1, preview, error, held) ==
		Status::LimitExceeded
	);
	CHECK(std::get<ImageArray>(preview).Images == before);
	CHECK(provider.Calls == 2);
}
TEST_CASE(
	"Preview preserves nested references and heterogeneous source surfaces", "[studio][preview-result]"
) {
	using namespace studio::detail;
	Image depth{2, 1, std::vector<uint8_t>(8), 0, SurfaceFormat::R32Float};
	REQUIRE(StoreSurfacePixel(depth, 0, 0, {0.25, 0, 0, 1}));
	REQUIRE(StoreSurfacePixel(depth, 1, 0, {0.75, 0, 0, 1}));
	depth.Hash = SurfaceHash(depth);
	ImageArray data{
		{Pixel(10), depth},
		{{size_t{0}}, {std::vector<ImageArrayItem>{{size_t{1}}, {size_t{0}}, {size_t{1}}}}}
	};
	const auto items = data.Items;
	const auto pixels = data.Images;
	ImageGraphPreviewSequence sequence;
	Diagnostic error;
	auto key = PreviewKey();
	REQUIRE(sequence.Admit(data, key, Limits::MaximumEvaluationBytes, error));
	sequence.Publish(std::move(data), std::move(key));
	CHECK(sequence.Selected().Image->Pixels[0] == 10);
	sequence.Indices[0] = 3;
	CHECK(sequence.Selected().Branch);
	CHECK_FALSE(sequence.Selected().Image);
	sequence.Levels = 2;
	sequence.Indices[1] = 5;
	REQUIRE(sequence.Selected().Image);
	CHECK(sequence.Selected().Image->Format == SurfaceFormat::R32Float);
	CHECK(sequence.Selected().Image->Pixels == pixels[1].Pixels);
	CHECK(sequence.Selected().Image->Width == 2);
	CHECK(sequence.Data.Items == items);
	CHECK(sequence.Data.Images == pixels);
	auto changed = PreviewKey(2);
	CHECK_FALSE(sequence.Matches(changed));
	changed = PreviewKey();
	changed.Identity.InputRevision = 2;
	CHECK_FALSE(sequence.Matches(changed));
	changed = PreviewKey();
	changed.Frame = {8};
	CHECK_FALSE(sequence.Matches(changed));
	changed = PreviewKey();
	changed.Identity.Owner = engine::core::Name("other");
	CHECK_FALSE(sequence.Matches(changed));
	changed = PreviewKey();
	changed.PlaybackObservation = 1;
	CHECK_FALSE(sequence.Matches(changed));
	sequence.Invalidate();
	CHECK_FALSE(sequence.Matches(PreviewKey()));
	CHECK(sequence.Selected().Image->Pixels == pixels[1].Pixels);
}
TEST_CASE("Preview admits exact old new retained capacities atomically", "[studio][preview-result]") {
	using namespace studio::detail;
	ImageGraphPreviewSequence sequence;
	ImageArray first{{Pixel(10)}, {{size_t{0}}}};
	Diagnostic error;
	auto key = PreviewKey();
	REQUIRE(sequence.Admit(first, key, Limits::MaximumEvaluationBytes, error));
	sequence.Publish(std::move(first), std::move(key));
	ImageArray next{{Pixel(20), Pixel(30)}, {{size_t{0}}, {size_t{1}}}};
	next.Images.reserve(23);
	next.Items.reserve(41);
	next.Images[0].Pixels.reserve(307);
	auto nextKey = PreviewKey(2);
	const uint64_t exact =
		sequence.RetainedBytes() + ImageGraphSequenceBytes(next) + nextKey.Identity.Output.capacity();
	CHECK_FALSE(sequence.Admit(next, nextKey, exact - 1, error));
	CHECK(sequence.Selected().Image->Pixels[0] == 10);
	CHECK(sequence.Matches(PreviewKey()));
	REQUIRE(sequence.Admit(next, nextKey, exact, error));
	sequence.Publish(std::move(next), std::move(nextKey));
	CHECK(sequence.Selected().Image->Pixels[0] == 20);
	ImageArray malformed{{Pixel(55)}, {{size_t{1}}}};
	CHECK_FALSE(sequence.Admit(malformed, PreviewKey(), Limits::MaximumEvaluationBytes, error));
	CHECK(error.Code == Status::InvalidOutput);
	CHECK(sequence.Selected().Image->Pixels[0] == 20);
	ImageArray empty;
	auto emptyKey = PreviewKey(3);
	REQUIRE(sequence.Admit(empty, emptyKey, Limits::MaximumEvaluationBytes, error));
	sequence.Publish(std::move(empty), std::move(emptyKey));
	CHECK(sequence.HaveSequence);
	CHECK_FALSE(sequence.Selected().Image);
	CHECK(sequence.Selected().Count == 0);
}
TEST_CASE(
	"Preview cache query includes retained metadata and preserves pixel-only allowance",
	"[studio][preview-result]"
) {
	studio::ImageGraphPreviewCache cache;
	CHECK(cache.HeldBytes() == 0);
	CHECK(cache.RetainedBytes() == sizeof(cache));
	Image image = Pixel(5);
	image.Pixels.reserve(273);
	REQUIRE(cache.Store(1, 0, 0, image));
	const auto *stored = cache.Find(1, 0, 0);
	REQUIRE(stored);
	CHECK(cache.HeldBytes() == stored->Pixels.capacity());
	CHECK(cache.RetainedBytes() > sizeof(cache) + cache.HeldBytes());
	const auto before = cache.RetainedBytes();
	REQUIRE(cache.Store(1, 1, 0, Pixel(6)));
	CHECK(cache.RetainedBytes() > before);
}
TEST_CASE("Sequence nested depth uses the evaluator bound without flattening", "[studio][preview-result]") {
	using namespace studio::detail;
	ImageArray data{{Pixel(9)}, {{size_t{0}}}};
	for (size_t i = 0; i < Limits::MaximumArrayDepth; ++i) {
		std::vector<ImageArrayItem> parent;
		parent.push_back({std::move(data.Items)});
		data.Items = std::move(parent);
	}
	Diagnostic error;
	REQUIRE(ValidateImageGraphSequence(data, error));
	std::array<uint64_t, Limits::MaximumArrayDepth + 1> indices{};
	const auto selected = ImageGraphPreviewSequence::Select(data, indices);
	REQUIRE(selected.Image);
	CHECK(selected.Image->Pixels[0] == 9);
	std::vector<ImageArrayItem> parent;
	parent.push_back({std::move(data.Items)});
	data.Items = std::move(parent);
	CHECK_FALSE(ValidateImageGraphSequence(data, error));
	CHECK(error.Code == Status::LimitExceeded);
}

TEST_CASE(
	"Pending replacement retains selected completed row until atomic new owner publication",
	"[studio][preview-result]"
) {
	using namespace studio::detail;
	ImageGraphPreviewSequence sequence;
	ImageArray first{{Pixel(10), Pixel(11)}, {{size_t{0}}, {size_t{1}}}};
	Diagnostic error;
	auto key = PreviewKey();
	REQUIRE(sequence.Admit(first, key, Limits::MaximumEvaluationBytes, error));
	sequence.Publish(std::move(first), std::move(key));
	sequence.Indices[0] = 3;
	ImageArray next{{Pixel(20), Pixel(21)}, {{size_t{0}}, {size_t{1}}}};
	auto sameOwner = PreviewKey();
	sameOwner.Frame = {8};
	CHECK(sequence.SelectReplacement(next, sameOwner).Image->Pixels[0] == 21);
	auto newOwner = PreviewKey(2);
	CHECK(sequence.SelectReplacement(next, newOwner).Image->Pixels[0] == 20);
	sequence.Invalidate();
	CHECK(sequence.Selected().Image->Pixels[0] == 11);
	CHECK(sequence.Indices[0] == 3);
	CHECK_FALSE(sequence.Admit(next, newOwner, sequence.RetainedBytes(), error));
	CHECK(sequence.Selected().Image->Pixels[0] == 11);
	REQUIRE(sequence.Admit(next, newOwner, Limits::MaximumEvaluationBytes, error));
	sequence.Publish(std::move(next), std::move(newOwner));
	CHECK(sequence.Indices[0] == 0);
	CHECK(sequence.Selected().Image->Pixels[0] == 20);
}
