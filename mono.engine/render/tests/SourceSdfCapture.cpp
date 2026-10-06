#include "SourceSdf.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
TEST_SUITE_ID("engine.render.source_sdf_capture")
TEST_DEPENDS("engine.imagegraph.source_sdf_host")
TEST_CASE("SDF completion packs raw surfaces and preserves receipts on refusal", "[render][sdf-capture]") {
	using namespace engine::render::imagegraph;
	using namespace engine::imagegraph;
	SourceSdfRequest request;
	request.Width = 2;
	request.Height = 1;
	std::array<std::byte, 8> raw{
		std::byte{255},
		std::byte{0},
		std::byte{128},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255}
	};
	HostNodeCapture receipt;
	request.Format = engine::assets::TextureFormat::RGBA4_UNORM;
	REQUIRE(CompleteSourceSdfCapture(request, raw, 1 << 20, receipt));
	REQUIRE(receipt.Images.size() == 1);
	CHECK(receipt.Images[0].Port == "surface_out");
	CHECK(receipt.Images[0].Data.Pixels == std::vector<uint8_t>{0x0f, 0xf8, 0xf0, 0xf0});
	const auto packed = receipt.Images[0].Data.Pixels;
	REQUIRE(CompleteSourceSdfCapture(request, std::as_bytes(std::span(packed)), 1 << 20, receipt, true));
	CHECK(receipt.Images[0].Data.Pixels == packed);
	const auto old = receipt.Images[0].Data;
	CHECK_FALSE(CompleteSourceSdfCapture(request, std::span(raw).first(4), 1 << 20, receipt));
	CHECK(receipt.Images[0].Data == old);
	CHECK_FALSE(CompleteSourceSdfCapture(request, raw, 1, receipt));
	CHECK(receipt.Images[0].Data == old);
	request.Format = engine::assets::TextureFormat::R8;
	REQUIRE(CompleteSourceSdfCapture(request, raw, 1 << 20, receipt));
	CHECK(receipt.Images[0].Data.Pixels == std::vector<uint8_t>{255, 0});
	request.Format = engine::assets::TextureFormat::RGBA8_LINEAR;
	REQUIRE(CompleteSourceSdfCapture(request, raw, 1 << 20, receipt));
	CHECK(receipt.Images[0].Data.Pixels == std::vector<uint8_t>{255, 0, 128, 255, 0, 255, 0, 255});
	request.Format = engine::assets::TextureFormat::R32_FLOAT;
	const std::array<float, 2> samples{.25f, .75f};
	REQUIRE(CompleteSourceSdfCapture(request, std::as_bytes(std::span(samples)), 1 << 20, receipt));
	const auto floatOld = receipt.Images[0].Data;
	const std::array<float, 2> bad{.25f, std::numeric_limits<float>::infinity()};
	CHECK_FALSE(CompleteSourceSdfCapture(request, std::as_bytes(std::span(bad)), 1 << 20, receipt));
	CHECK(receipt.Images[0].Data == floatOld);
}
TEST_CASE("resolved SDF controls own inputs and enforce replacement budgets", "[render][sdf-capture]") {
	using namespace engine::render::imagegraph;
	using namespace engine::imagegraph;
	Node node{"rm", "pc.rm_render", "", {}, {}};
	SdfValue object;
	object.Data.emplace().Shapes.emplace_back();
	object.Data->Shapes[0].Shape = 200;
	object.Data->Shapes[0].Identity = "sphere";
	object.Data->Operations.push_back({0, 0});
	std::vector<AuthoredValue> inputs{
		{"sdf_object", std::move(object)}, {"dimension", Vector2{.5, .25}}, {"dimension_unit", EnumValue{1}}
	};
	EvaluationRequest clock;
	HostNodeInvocation invocation{
		node,
		clock,
		inputs,
		{},
		1 << 20,
		nullptr,
		SurfaceFormat::RGBA8Unorm,
		1,
		SourceCameraEvaluationPolicy{64, 32}
	};
	SourceSdfRequest request;
	std::string failure;
	REQUIRE(BuildSourceSdfRequest(invocation, false, request, failure));
	CHECK(request.Width == 32);
	CHECK(request.Height == 8);
	CHECK(request.Object.Data.operator->() != std::get<SdfValue>(inputs[0].Data).Data.operator->());
	const auto saved = request;
	invocation.MaximumOperationBytes = 1;
	CHECK_FALSE(BuildSourceSdfRequest(invocation, false, request, failure));
	CHECK_FALSE(failure.empty());
	CHECK(request == saved);
	invocation.MaximumOperationBytes = 1 << 20;
	invocation.CameraPolicy->DimensionLinked = true;
	REQUIRE(BuildSourceSdfRequest(invocation, false, request, failure));
	CHECK(request.Width == 1);
	CHECK(request.Height == 1);
	invocation.CameraPolicy.reset();
	CHECK_FALSE(BuildSourceSdfRequest(invocation, false, request, failure));
}

TEST_CASE("selected SDF previews count snapshot and previous request residency", "[render][sdf-capture]") {
	using namespace engine::imagegraph;
	using namespace engine::render::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"shape", "pc.rm_primitive", "", {}, {{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}}}
	};
	document.Outputs = {{"out", "shape", "sdf_object"}};
	Plan plan;
	Diagnostic diagnostic;
	EvaluationSnapshot snapshot;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateNodeInputs(document, plan, "shape", {}, snapshot, diagnostic) == Status::Ok);
	SourceSdfRequest request;
	request.Environment = Image{64, 64, std::vector<uint8_t>(64 * 64 * 4)};
	const auto saved = request;
	const uint64_t refusedBudget = snapshot.RetainedBytes() + SourceSdfSourceBytes(request);
	CHECK_FALSE(BuildSourceSdfRequest(
		document.Nodes[0],
		snapshot,
		SourceCameraEvaluationPolicy{},
		"surface_out",
		0,
		false,
		request,
		diagnostic,
		refusedBudget
	));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(request == saved);
	REQUIRE(BuildSourceSdfRequest(
		document.Nodes[0],
		snapshot,
		SourceCameraEvaluationPolicy{},
		"surface_out",
		0,
		false,
		request,
		diagnostic
	));
	CHECK(request.Width == 4);
	CHECK(request.Height == 4);
	REQUIRE(request.Object.Data);
}
