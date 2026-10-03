#include "../src/PixelBuilderPayload.hpp"
#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/PcxExpression.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_nine_slice_nodes")
using namespace engine::imagegraph;
namespace {
	DynamicSurfaceValue Recipe(Image source, Vector4 splice = {}) {
		DynamicSurfaceValue result;
		auto &data = result.Data.emplace();
		data.OwnerNodeId = "nine";
		data.BaseDimension = {double(source.Width), double(source.Height)};
		data.NineSlice.emplace(SourceNineSliceRecipe{std::move(source), splice});
		return result;
	}
}
TEST_CASE(
	"Nine Slice image and dynamic outputs preserve distinct dimensions and owned original input",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	Image source{3, 3, std::vector<uint8_t>(36, 128), 0};
	auto run = imagegraph_test::RunNode(
		"pc.9_slice",
		{{"surface_in", &source}},
		{{"dimension", Vector2{5, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"splice", Vector4{1, 1, 1, 1}},
		 {"splice_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 5);
	CHECK(run.Output().Height == 4);
	CHECK(run.Output().Pixels[0] == 64);
	CHECK(run.Output().Pixels[3] == 64);
	const auto *output = run.OutputValue("dyna_surf");
	REQUIRE(output);
	const auto *recipe = std::get_if<DynamicSurfaceValue>(output);
	REQUIRE(recipe);
	REQUIRE(recipe->Data);
	CHECK(recipe->Data->BaseDimension == Vector2{3, 3});
	source.Pixels[0] = 17;
	CHECK(recipe->Data->NineSlice->Source.Pixels[0] == 128);
	CHECK_FALSE(engine::imagegraph::detail::ValidValuePayload(*output, false));
}
TEST_CASE(
	"Nine Slice actual compiled persistence regenerates owned dynamic recipe",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{3, 3}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}}},
		{"nine",
		 "pc.9_slice",
		 "",
		 {},
		 {{"dimension", Vector2{5, 4}},
		  {"dimension_unit", EnumValue{0}},
		  {"splice", Vector4{1, 1, 1, 1}},
		  {"splice_unit", EnumValue{0}}}}
	};
	document.Links = {{"source", "surface_out", "nine", "surface_in"}};
	document.Outputs = {{"image", "nine", "surface_out"}, {"recipe", "nine", "dyna_surf"}};
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	auto status = EvaluateValue(restored, plan, "recipe", {}, value, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *recipe = std::get_if<DynamicSurfaceValue>(&value.Data);
	REQUIRE(recipe);
	REQUIRE(recipe->Data);
	CHECK(recipe->Data->BaseDimension == Vector2{3, 3});
	Image selected;
	REQUIRE(Evaluate(restored, plan, "image", {}, selected, diagnostic) == Status::Ok);
	const auto previous = selected;
	CHECK(Evaluate(restored, plan, "image", {}, selected, diagnostic, 1) == Status::LimitExceeded);
	CHECK(selected == previous);
	for (Vector2 dimension : {Vector2{4, 4}, Vector2{7, 5}}) {
		Image image;
		REQUIRE(
			engine::imagegraph::detail::RasterizePixelBuilder(*recipe, dimension, image, diagnostic) ==
			Status::Ok
		);
		CHECK(image.Width == uint32_t(dimension.X));
		CHECK(image.Height == uint32_t(dimension.Y));
		CHECK(image.Pixels[0] == 255);
	}
}
TEST_CASE(
	"Nine Slice PCX draw applies tint before staging and changes subsequent draw blend",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	auto recipe = Recipe({1, 1, {255, 128, 64, 128}, 0});
	PcxExpressionValue program;
	Diagnostic diagnostic;
	REQUIRE(
		CompilePcxProgram("draw(nine,0,0,1,1,0,16777088,0.5)\ndraw(raw,1,0)", program, diagnostic) ==
		Status::Ok
	);
	const std::array parameters{
		AuthoredValue{"nine", recipe}, AuthoredValue{"raw", SurfaceValue{Image{1, 1, {255, 0, 0, 128}, 0}}}
	};
	Image target{2, 1, std::vector<uint8_t>(8), 0};
	EvaluationRequest request;
	PcxExecutionContext context{request, parameters, nullptr, {32, 32}, {}};
	context.Target = &target;
	context.DrawBlend = PcxDrawBlend::Override;
	PcxExecutionResult result;
	const auto status = ExecutePcxExpression(program, context, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(target.Pixels == std::vector<uint8_t>{32, 32, 16, 16, 128, 0, 0, 64});
}
TEST_CASE(
	"Nine Slice PB surface performs its dynamic Normal draw over clear canvas",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	auto recipe = Recipe({1, 1, {255, 0, 0, 128}, 0});
	auto run = imagegraph_test::RunNode("pc.pb_draw_surface", {}, {{"surface", recipe}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output("surface").Pixels[0] == 128);
	CHECK(run.Output("surface").Pixels[3] == 64);
}
TEST_CASE(
	"Nine Slice rerender failure preserves previous image and charges retained source slack",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	auto recipe = Recipe({1, 1, {255, 0, 0, 255}, 0});
	recipe.Data->NineSlice->Source.Pixels.reserve(2 * 1024 * 1024);
	Image image{1, 1, {1, 2, 3, 4}, 0};
	const auto before = image;
	Diagnostic diagnostic;
	CHECK(
		engine::imagegraph::detail::RasterizeSourceNineSlice(
			recipe, {2, 2}, {1, 1, 1, 1}, image, diagnostic, 1024 * 1024
		) == Status::LimitExceeded
	);
	CHECK(image == before);
	auto copy = recipe;
	copy.Data->NineSlice->Source.Pixels[0] = 17;
	CHECK(recipe.Data->NineSlice->Source.Pixels[0] == 255);
}
TEST_CASE(
	"Nine Slice Reference splice half even getter preserves physical source borders",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	Image source{2, 2, std::vector<uint8_t>(16, 255), 0};
	auto run = imagegraph_test::RunNode(
		"pc.9_slice",
		{{"surface_in", &source}},
		{{"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"splice", Vector4{.25, .75, .25, .75}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto *recipe = std::get_if<DynamicSurfaceValue>(run.OutputValue("dyna_surf"));
	REQUIRE(recipe);
	CHECK(recipe->Data->NineSlice->Splice == Vector4{0, 2, 0, 2});
}
TEST_CASE(
	"Nine Slice actual processor dimension rows publish complete image and owned recipe arrays",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	Document document;
	document.FormatVersion = 9;
	Node dimensions{"sizes", "pc.array", "", {}, {}, {}};
	dimensions.DynamicInputs = {
		{"input_0", ValueType::Vector2, Vector2{4, 4}}, {"input_1", ValueType::Vector2, Vector2{5, 6}}
	};
	document.Nodes = {
		dimensions,
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{3, 3}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}}},
		{"nine",
		 "pc.9_slice",
		 "",
		 {},
		 {{"dimension_unit", EnumValue{0}}, {"splice", Vector4{1, 1, 1, 1}}, {"splice_unit", EnumValue{0}}}}
	};
	document.Links = {
		{"source", "surface_out", "nine", "surface_in"}, {"sizes", "array", "nine", "dimension"}
	};
	document.Outputs = {{"images", "nine", "surface_out"}, {"recipes", "nine", "dyna_surf"}};
	Diagnostic diagnostic;
	Plan plan;
	auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	ImageArray images;
	status = EvaluateArray(document, plan, "images", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Width == 4);
	CHECK(images.Images[1].Width == 5);
	CHECK(images.Images[1].Height == 6);
	EvaluatedValue recipes;
	status = EvaluateValue(document, plan, "recipes", {}, recipes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *array = std::get_if<ArrayValue>(&recipes.Data);
	REQUIRE(array);
	CHECK(array->ElementType == ValueType::DynamicSurface);
}
TEST_CASE(
	"Nine Slice constructor outer Bicubic draw retains original shader sample dimensions",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	const Image source{3, 1, {20, 0, 0, 255, 40, 0, 0, 255, 60, 0, 0, 255}, 0};
	auto run = imagegraph_test::RunNode(
		"pc.9_slice",
		{{"surface_in", &source}},
		{{"dimension", Vector2{4, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"splice", Vector4{1, 0, 1, 0}},
		 {"splice_unit", EnumValue{0}},
		 {"interpolate", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(
		run.Output().Pixels ==
		std::vector<uint8_t>{22, 0, 0, 255, 37, 0, 0, 255, 43, 0, 0, 255, 58, 0, 0, 255}
	);
}
TEST_CASE(
	"Padding inactive dynamic input bypass retains its owned recipe identity",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	auto recipe = Recipe({1, 1, {255, 0, 0, 128}, 0});
	auto run = imagegraph_test::RunNode("pc.padding", {}, {{"surface_in", recipe}, {"active", false}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto *value = run.OutputValue("surface_out");
	REQUIRE(value);
	const auto *copy = std::get_if<DynamicSurfaceValue>(value);
	REQUIRE(copy);
	CHECK(*copy == recipe);
	CHECK(copy->Data.operator->() != recipe.Data.operator->());
}
TEST_CASE(
	"Nine Slice nonraw dynamic input preserves source previous-output refusal on both ports",
	"[imagegraph][source_2d][source_nine_slice]"
) {
	auto recipe = Recipe({1, 1, {255, 0, 0, 255}, 0});
	const auto *entry = FindCatalogueEntry("pc.9_slice");
	REQUIRE(entry);
	Node node{"nine", "pc.9_slice", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values.emplace_back("surface_in", recipe);
	const auto execute = engine::imagegraph::detail::FindExecutor("pc.9_slice");
	REQUIRE(execute);
	REQUIRE(execute(context));
	CHECK(context.FailureCode == Status::Ok);
	REQUIRE(context.OutputDiagnostic("surface_out"));
	REQUIRE(context.OutputDiagnostic("dyna_surf"));
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputValues.empty());
}
