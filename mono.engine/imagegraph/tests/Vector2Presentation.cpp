#include "../src/ProcessorBatch.hpp"

#include <engine/imagegraph/Vector2Presentation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.vector2_presentation")
TEST_DEPENDS("engine.imagegraph.document")
using namespace engine::imagegraph;
namespace {
	Document PresentationDocument() {
		Document document;
		document.FormatVersion = 8;
		document.Nodes.push_back(
			{"vector", "pc.vector2", "", {}, {{"x", 2.5}, {"y", -3.5}, {"integer", true}}}
		);
		document.Outputs.push_back({"out", "vector", "vector"});
		return document;
	}
	ArrayValue Numbers(std::initializer_list<double> values) {
		ArrayValue array{ValueType::Scalar, {}};
		for (double value : values)
			array.Elements.emplace_back(value);
		return array;
	}
	void Resolve(const Document &document, const EvaluationRequest &request, Vector2Presentation &result) {
		Diagnostic diagnostic;
		const auto retained = Write(document);
		const Status status = ResolveVector2Presentation(document, "vector", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(Write(document) == retained);
	}
}
TEST_CASE(
	"Vector2 presentation resolves rounded position linked timeline and source choice controls",
	"[imagegraph][vector2_presentation]"
) {
	Document document = PresentationDocument();
	document.Project = ProjectSettings{320, 200};
	document.Nodes[0].Values.insert(
		document.Nodes[0].Values.end(),
		{{"display_type", 9.0},
		 {"gizmo_style", .5},
		 {"gizmo_shape", EnumValue{1}},
		 {"show_on_global", true},
		 {"relative_unit", true},
		 {"gizmo_offset", Vector2{10, 20}},
		 {"gizmo_scale", 3.0},
		 {"gizmo_size", Vector2{16, 24}}}
	);
	Vector2Presentation result;
	Resolve(document, {}, result);
	CHECK(result.X == 2);
	CHECK(result.Y == -4);
	CHECK(result.Integer);
	CHECK(result.DisplayType == 1);
	CHECK(result.Style == .5);
	CHECK(result.Shape == 1);
	CHECK(result.ShowOnGlobal);
	CHECK(result.RelativeUnit);
	CHECK(result.Offset == Vector2{10, 20});
	CHECK(result.Size == Vector2{16, 24});
	CHECK(result.Scale == 3);
	CHECK(result.ProjectWidth == 320);
	CHECK(result.ProjectHeight == 200);
	CHECK(result.ProcessorCount == 1);
	CHECK_FALSE(result.XLinked);
	CHECK_FALSE(result.YLinked);
	document.Nodes.push_back({"source", "pc.number", "", {}, {{"value", 0.0}}});
	document.Links.push_back({"source", "number", "vector", "x"});
	document.Keyframes = {{"source", "value", 0, 0.0, "linear"}, {"source", "value", 10, 10.0, "step"}};
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 5;
	request.Subframe = .5;
	Resolve(parsed, request, result);
	CHECK(result.X == 6);
	CHECK(result.XLinked);
	CHECK_FALSE(result.YLinked);
	request.Tick = 0;
	request.Subframe = 0;
	Resolve(parsed, request, result);
	CHECK(result.X == 0);
	request.Tick = 5;
	request.Subframe = .5;
	Resolve(parsed, request, result);
	CHECK(result.X == 6);
}
TEST_CASE(
	"Vector2 presentation counts real processor rows from axes and presentation controls",
	"[imagegraph][vector2_presentation]"
) {
	for (int64_t mode = 0; mode < 4; ++mode) {
		CAPTURE(mode);
		Document document = PresentationDocument();
		document.Nodes[0].Values = {
			{"x", Numbers({1, 2})},
			{"y", 4.0},
			{"gizmo_scale", Numbers({3, 4, 5})},
			{"attribute_array_process", EnumValue{mode}},
			{"gizmo_style", EnumValue{2}}
		};
		Vector2Presentation result;
		Resolve(document, {}, result);
		CHECK(result.ProcessorCount == (mode < 2 ? 3 : 6));
		CHECK_FALSE(result.Sprite);
	}
	Document document = PresentationDocument();
	document.Nodes[0].Values.push_back({"gizmo_scale", Numbers({1, 2, 3})});
	Vector2Presentation result;
	Resolve(document, {}, result);
	CHECK(result.ProcessorCount == 3);
}
TEST_CASE(
	"Vector2 sprite capture owns bounded pixels only for unbatched Sprite style",
	"[imagegraph][vector2_presentation]"
) {
	Document document = PresentationDocument();
	document.Nodes.push_back(
		{"sprite",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{3}}, {"colour", Colour{12, 34, 56, 255}}}}
	);
	document.Links.push_back({"sprite", "image", "vector", "gizmo_sprite"});
	EvaluationRequest request;
	request.MaximumImageDimension = 128;
	Vector2Presentation result;
	Resolve(document, request, result);
	CHECK_FALSE(result.Sprite);
	document.Nodes[0].Values.push_back({"gizmo_style", EnumValue{2}});
	Resolve(document, request, result);
	REQUIRE(result.Sprite);
	CHECK(result.Sprite->Width == 2);
	CHECK(result.Sprite->Height == 3);
	CHECK(result.Sprite->Pixels.size() == 24);
	const auto pixels = result.Sprite->Pixels;
	document.Nodes[0].Values[0].Data = Numbers({1, 2});
	Resolve(document, request, result);
	CHECK(result.ProcessorCount == 2);
	CHECK_FALSE(result.Sprite);
	document.Nodes[0].Values[0].Data = 2.5;
	Resolve(document, request, result);
	REQUIRE(result.Sprite);
	document.Nodes[1].Values[0].Data = int64_t{129};
	Diagnostic diagnostic;
	CHECK(
		ResolveVector2Presentation(document, "vector", request, result, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(result.Sprite);
	CHECK(result.Sprite->Pixels == pixels);
	CHECK(ResolveVector2Presentation(document, "sprite", request, result, diagnostic) == Status::UnknownNode);
	CHECK(result.Sprite->Pixels == pixels);
}

TEST_CASE(
	"Processor observation failure discards all staged outputs and presentation remains atomic",
	"[imagegraph][vector2_presentation]"
) {
	using namespace engine::imagegraph::detail;
	Document document = PresentationDocument();
	const auto *entry = FindCatalogueEntry("pc.vector2");
	REQUIRE(entry);
	EvaluationRequest request;
	NodeContext context(document.Nodes[0], *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	for (const auto &input : entry->Inputs) {
		if (auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	}
	const auto refuse = [](NodeContext &selected, void *) {
		return selected.Fail(Status::LimitExceeded, "presentation refused before publication");
	};
	CHECK_FALSE(RunProcessorBatch(context, FindExecutor("pc.vector2"), refuse));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
	document.Nodes[0].Values.push_back({"gizmo_scale", Numbers({})});
	Vector2Presentation result;
	result.X = 42;
	Diagnostic diagnostic;
	CHECK(
		ResolveVector2Presentation(document, "vector", request, result, diagnostic) == Status::InvalidValue
	);
	CHECK(diagnostic.Port == "gizmo_scale");
	CHECK(result.X == 42);
}
