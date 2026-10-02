#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_conversion")
using namespace engine::imagegraph;
namespace {
	Document NumberImage(ArrayValue numbers, int64_t mode = 0) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"convert",
			 "pc.interpret_number",
			 "",
			 {},
			 {{"number", std::move(numbers)},
			  {"mode", EnumValue{mode}},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		document.Outputs = {{"image", "convert", "surface_out"}};
		return document;
	}
	Image Render(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Image image;
		const auto status = Evaluate(document, plan, "image", EvaluationRequest{}, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	Document Oklch(Colour colour) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"colour", "pc.color", "", {}, {{"color", colour}}}, {"convert", "pc.color_to_oklch", "", {}, {}}
		};
		document.Links = {{"colour", "color", "convert", "color"}};
		document.Outputs = {
			{"l", "convert", "lightness"}, {"c", "convert", "chroma"}, {"h", "convert", "hue"}
		};
		return document;
	}
}
TEST_CASE(
	"Interpret Number flattens typed and general source arrays across shader batches",
	"[imagegraph][source_conversion]"
) {
	ArrayValue numbers{ValueType::Scalar, {}};
	for (size_t i = 0; i < 259; ++i)
		numbers.Elements.emplace_back(double(i % 3) / 2);
	auto document = NumberImage(numbers);
	const auto image = Render(document);
	REQUIRE(image.Width == 259);
	REQUIRE(image.Height == 1);
	for (size_t i = 0; i < 259; ++i)
		CHECK(image.Pixels[4 * i] == (i % 3 == 0 ? 0 : i % 3 == 1 ? 128 : 255));
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(Render(restored) == image);
	ArrayValue nested{ValueType::Any, {}};
	nested.Items = {
		{ElementValue{false}},
		{std::vector<SourceArrayItem>{{ElementValue{Vector2{.25, .75}}}, {ElementValue{int64_t{1}}}}}
	};
	const auto flattened = Render(NumberImage(nested));
	CHECK(flattened.Width == 4);
	CHECK(
		flattened.Pixels ==
		std::vector<uint8_t>{0, 0, 0, 255, 64, 64, 64, 255, 191, 191, 191, 255, 255, 255, 255, 255}
	);
}
TEST_CASE(
	"Interpret Number resolves palette gradient wrapping and mapped gradients",
	"[imagegraph][source_conversion]"
) {
	ArrayValue numbers{ValueType::Scalar, {0., 1.9, 2., 3.}};
	auto document = NumberImage(numbers, 1);
	document.Nodes[0].Values.push_back(
		{"palette", ArrayValue{ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 128}}}}
	);
	const auto palette = Render(document);
	CHECK(
		palette.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 128, 255, 0, 0, 255, 0, 255, 0, 128}
	);
	ArrayValue largePalette{ValueType::Colour, {}};
	largePalette.Elements.assign(256, Colour{255, 0, 0, 255});
	largePalette.Elements.emplace_back(Colour{0, 0, 255, 255});
	auto truncated = NumberImage(ArrayValue{ValueType::Scalar, {256.}}, 1);
	truncated.Nodes[0].Values.push_back({"palette", std::move(largePalette)});
	CHECK(Render(truncated).Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	ArrayValue generalPalette{ValueType::Any, {}};
	generalPalette.Items = {{ElementValue{int64_t{0xFF0000FF}}}, {ElementValue{Colour{0, 255, 0, 128}}}};
	document.Nodes[0].Values.back().Data = generalPalette;
	CHECK(Render(document).Pixels == palette.Pixels);

	document = NumberImage(ArrayValue{ValueType::Scalar, {-.25, 0., .25, .75, 1.}}, 2);
	document.Nodes[0].Values.push_back(
		{"gradient", Gradient{0, {{0, Colour{0, 0, 0, 255}}, {1, Colour{255, 255, 255, 255}}}}}
	);
	document.Nodes[0].Values.push_back({"shift", .25});
	const auto gradient = Render(document);
	CHECK(gradient.Pixels[0] == 0);
	CHECK(gradient.Pixels[4] == 64);
	CHECK(gradient.Pixels[8] == 128);
	CHECK(gradient.Pixels[12] == 0);
	CHECK(gradient.Pixels[16] == 64);
	document.Nodes.insert(
		document.Nodes.begin(), Node{"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}}
	);
	document.Nodes[1].Values.push_back({"gradient_mapped", true});
	document.Nodes[1].Values.push_back({"gradient_map_range", Vector4{0, .5, 1, .5}});
	document.Links = {{"map", "image", "convert", "gradient_map"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image map{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0};
	std::array<RequestImageSource, 1> sources{{{"map", map}}};
	Image mapped;
	REQUIRE(
		Evaluate(document, plan, "image", EvaluationRequest{.ImageSources = sources}, mapped, diagnostic) ==
		Status::Ok
	);
	CHECK(mapped.Pixels[0] == 255);
	CHECK(mapped.Pixels[8] == 128);
	CHECK(mapped.Pixels[10] == 128);
}
TEST_CASE(
	"Interpret Number processor rows preserve images and all source surface formats",
	"[imagegraph][source_conversion]"
) {
	auto document = NumberImage(ArrayValue{ValueType::Scalar, {}});
	document.Nodes.insert(
		document.Nodes.begin(),
		Node{"numbers", "pc.equation", "", {}, {{"equation", std::string{"[[0,1],[1,0]]"}}}}
	);
	document.Nodes[1].Values.erase(document.Nodes[1].Values.begin());
	document.Links = {{"numbers", "result", "convert", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(document, plan, "image", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	CHECK(rows.Images[0].Pixels[0] == 0);
	CHECK(rows.Images[1].Pixels[0] == 255);
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto typed = NumberImage(ArrayValue{ValueType::Scalar, {.25}});
		typed.Nodes[0].Values.back().Data = EnumValue{depth};
		const auto result = Render(typed);
		CHECK(result.Format == *SourceSurfaceFormat(depth));
		std::array<double, 4> sample;
		REQUIRE(LoadSurfacePixel(result, 0, 0, sample));
		CHECK(sample[0] == Catch::Approx(.25).margin(depth == 2 ? .04 : .005));
	}
}
TEST_CASE(
	"Interpret Number guards undefined shader indices empty handles and atomic budgets",
	"[imagegraph][source_conversion]"
) {
	for (const auto &[number, mode, range] :
		 std::array<std::tuple<double, int64_t, Vector2>, 2>{{{-1., 1, {0, 1}}, {.5, 0, {1, 1}}}}) {
		const auto run = imagegraph_test::RunNode(
			"pc.interpret_number",
			{},
			{{"number", ArrayValue{ValueType::Scalar, {number}}},
			 {"mode", EnumValue{mode}},
			 {"range", range},
			 {"attribute_color_depth", EnumValue{3}}}
		);
		CHECK_FALSE(run.Ok);
	}
	// The scalar EButton getter clamps 3 to Gradient, while array-selected 3 reaches the shader.
	const auto clamped = imagegraph_test::RunNode(
		"pc.interpret_number",
		{},
		{{"number", ArrayValue{ValueType::Scalar, {.5}}},
		 {"mode", EnumValue{3}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(clamped.Ok);
	CHECK(clamped.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	auto modes = NumberImage(ArrayValue{ValueType::Scalar, {.5}});
	modes.Nodes[0].Values[1].Data = ArrayValue{ValueType::Enum, {EnumValue{3}, EnumValue{0}}};
	Plan modePlan;
	Diagnostic modeDiagnostic;
	REQUIRE(Compile(modes, modePlan, modeDiagnostic) == Status::TypeMismatch);
	CHECK(modeDiagnostic.NodeId == "convert");
	CHECK(modeDiagnostic.Port == "mode");
	CHECK(modeDiagnostic.Message == "authored property has the wrong value type");
	// Runtime numeric rows come from a real Array producer, rather than an authored Enum-array literal.
	modes.Nodes[0].Values.erase(modes.Nodes[0].Values.begin() + 1);
	Node modeRows{"modes", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	modeRows.DynamicInputs = {{"input_0", ValueType::Scalar, 3.0}, {"input_1", ValueType::Scalar, 0.0}};
	modes.Nodes.insert(modes.Nodes.begin(), std::move(modeRows));
	modes.Links = {{"modes", "array", "convert", "mode"}};
	const auto modeStatus = Compile(modes, modePlan, modeDiagnostic);
	INFO(modeDiagnostic.Message);
	REQUIRE(modeStatus == Status::Ok);
	ImageArray modeImages;
	REQUIRE(EvaluateArray(modes, modePlan, "image", {}, modeImages, modeDiagnostic) == Status::Ok);
	REQUIRE(modeImages.Images.size() == 2);
	CHECK(modeImages.Images[0].Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	CHECK(modeImages.Images[1].Pixels == std::vector<uint8_t>{128, 128, 128, 255});

	for (int64_t mode : {int64_t{0}, int64_t{3}}) {
		const auto result = imagegraph_test::RunNode(
			"pc.interpret_matrix",
			{},
			{{"matrix", MatrixValue{1, 1, {.5}}},
			 {"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{1}},
			 {"attribute_color_depth", EnumValue{3}},
			 {"mode", EnumValue{mode}},
			 {"range", Vector2{1, 1}}}
		);
		CHECK_FALSE(result.Ok);
	}

	auto document = NumberImage(ArrayValue{ValueType::Scalar, {}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image previous{1, 1, {9, 8, 7, 6}, 0};
	CHECK(Evaluate(document, plan, "image", {}, previous, diagnostic) == Status::UnsupportedExecution);
	CHECK(previous.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	CHECK(diagnostic.Port == "surface_out");
	document = NumberImage(ArrayValue{ValueType::Scalar, {0., 1.}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(
		Evaluate(
			document, plan, "image", EvaluationRequest{.MaximumImageDimension = 1}, previous, diagnostic
		) == Status::LimitExceeded
	);
	CHECK(previous.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
	CHECK(Evaluate(document, plan, "image", {}, previous, diagnostic, 1) == Status::LimitExceeded);
	CHECK(previous.Pixels == std::vector<uint8_t>{9, 8, 7, 6});
}
TEST_CASE(
	"Color OKLCH matches pinned source constants and retains neutral siblings",
	"[imagegraph][source_conversion]"
) {
	const std::array<Colour, 4> colours{
		{{255, 0, 0, 0}, {0, 255, 0, 255}, {0, 0, 255, 128}, {17, 128, 255, 255}}
	};
	const std::array<std::array<double, 3>, 4> golden{
		{{.6279553606145515, .2576833077361569, 29.233885192342598},
		 {.8664396115356694, .2948272403370166, 142.49533888780996},
		 {.4520137183853429, .31321437166460125, 264.05202063805496},
		 {.6164776599252821, .20953861327479154, 256.7718786973535}}
	};
	for (size_t i = 0; i < colours.size(); ++i) {
		auto document = Oklch(colours[i]);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		size_t channel = 0;
		for (const auto *output : {"l", "c", "h"}) {
			EvaluatedValue value;
			REQUIRE(EvaluateValue(restored, plan, output, {}, value, diagnostic) == Status::Ok);
			CHECK(std::get<double>(value.Data) == Catch::Approx(golden[i][channel++]).epsilon(1e-12));
		}
	}
	for (const auto colour : {Colour{0, 0, 0, 255}, Colour{128, 128, 128, 255}, Colour{255, 255, 255, 255}}) {
		auto document = Oklch(colour);
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue value;
		REQUIRE(EvaluateValue(document, plan, "l", {}, value, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(document, plan, "c", {}, value, diagnostic) == Status::Ok);
		CHECK(std::get<double>(value.Data) < .0002);
		value.Data = std::string{"previous"};
		CHECK(EvaluateValue(document, plan, "h", {}, value, diagnostic) == Status::UnsupportedExecution);
		CHECK(std::get<std::string>(value.Data) == "previous");
		CHECK(diagnostic.NodeId == "convert");
		CHECK(diagnostic.Port == "hue");
	}
}
TEST_CASE(
	"Color OKLCH processor hue refusal preserves complete sibling rows and packed inputs",
	"[imagegraph][source_conversion]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"convert",
		 "pc.color_to_oklch",
		 "",
		 {},
		 {{"color",
		   ArrayValue{
			   ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{128, 128, 128, 255}, Colour{0, 0, 255, 255}}
		   }}}}
	};
	document.Outputs = {{"l", "convert", "lightness"}, {"h", "convert", "hue"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "l", {}, value, diagnostic) == Status::Ok);
	REQUIRE(std::get<ArrayValue>(value.Data).Elements.size() == 3);
	CHECK(EvaluateValue(document, plan, "h", {}, value, diagnostic) == Status::UnsupportedExecution);
	for (Value packed : {Value{int64_t{255}}, Value{255.}, Value{EnumValue{255}}, Value{true}}) {
		const auto result = imagegraph_test::RunNode("pc.color_to_oklch", {}, {{"color", packed}});
		REQUIRE(result.Ok);
		CHECK(std::get<double>(*result.OutputValue("hue")) == Catch::Approx(29.233885192342598));
	}
}
