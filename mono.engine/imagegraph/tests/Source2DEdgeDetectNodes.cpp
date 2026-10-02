#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.imagegraph.source_edge_detect")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image SolidImage(uint32_t width, uint32_t height, std::array<uint8_t, 4> colour) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * colour.size());
		for (size_t offset = 0; offset < image.Pixels.size(); offset += colour.size())
			for (size_t channel = 0; channel < colour.size(); ++channel)
				image.Pixels[offset + channel] = colour[channel];
		return image;
	}

	ArrayValue Sides(std::initializer_list<double> switches) {
		ArrayValue array;
		array.ElementType = ValueType::Scalar;
		for (double value : switches)
			array.Elements.emplace_back(value);
		return array;
	}
	Image Step() {
		Image image = SolidImage(3, 3, {0, 0, 0, 0});
		for (uint32_t y = 0; y < 3; ++y)
			for (uint32_t x = 1; x < 3; ++x) {
				const size_t at = (size_t(y) * 3 + x) * 4;
				image.Pixels[at] = 64;
				image.Pixels[at + 3] = 192;
			}
		return image;
	}
}
TEST_CASE("Edge Detect Sobel and Prewitt distance includes source alpha", "[imagegraph][source_2d]") {
	const Image image = Step();
	for (int64_t algorithm : {0, 1}) {
		const auto run =
			RunNode("pc.edge_detect", {{"surface_in", &image}}, {{"algorithm", EnumValue{algorithm}}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		// sqrt(64^2+192^2) rounds to202 after the source 4/3 kernel normalization.
		CHECK(run.Output().Pixels[16] == 202);
		CHECK(run.Output().Pixels[17] == 202);
		CHECK(run.Output().Pixels[18] == 202);
		CHECK(run.Output().Pixels[19] == 192);
	}
}
TEST_CASE("Edge Detect diagonal gradients cancel in literal vector distance", "[imagegraph][source_2d]") {
	Image image = SolidImage(3, 3, {0, 0, 0, 255});
	for (uint32_t y = 0; y < 3; ++y)
		for (uint32_t x = 0; x < 3; ++x)
			image.Pixels[(size_t(y) * 3 + x) * 4] = uint8_t((x + y) * 32);
	const auto run = RunNode("pc.edge_detect", {{"surface_in", &image}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[16] == 0);
	CHECK(run.Output().Pixels[19] == 255);
}
TEST_CASE(
	"Edge Detect keeps signed Laplacian and clamps neighbors before oversampling", "[imagegraph][source_2d]"
) {
	const Image image = Step();
	const auto first = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{2}}, {"oversample", EnumValue{1}}}
	);
	INFO(first.Message);
	REQUIRE(first.Ok);
	CHECK(first.Output().Pixels[12] == 96);
	CHECK(first.Output().Pixels[16] == 0);
	CHECK(first.Output().Pixels[19] == 192);
	for (int64_t oversample : {2, 3, 4, 6, 7, 8, 10, 11, 12}) {
		const auto run = RunNode(
			"pc.edge_detect",
			{{"surface_in", &image}},
			{{"algorithm", EnumValue{2}}, {"oversample", EnumValue{oversample}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == first.Output().Pixels);
	}
}
TEST_CASE(
	"Edge Detect neighbor switches retain exact positions and skip their center", "[imagegraph][source_2d]"
) {
	Image image = SolidImage(3, 3, {0, 0, 0, 128});
	image.Pixels[0] = 64;
	image.Pixels[1] = 128;
	image.Pixels[2] = 192;
	const auto topLeft = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}}, {"attribute_filter", Sides({1, 0, 0, 0, 0, 0, 0, 0, 0})}}
	);
	INFO(topLeft.Message);
	REQUIRE(topLeft.Ok);
	CHECK(topLeft.Output().Pixels[16] == 64);
	CHECK(topLeft.Output().Pixels[17] == 128);
	CHECK(topLeft.Output().Pixels[18] == 192);
	CHECK(topLeft.Output().Pixels[19] == 128);
	const auto bottomRight = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}}, {"attribute_filter", Sides({0, 0, 0, 0, 1, 0, 0, 0, 1})}}
	);
	INFO(bottomRight.Message);
	REQUIRE(bottomRight.Ok);
	CHECK(bottomRight.Output().Pixels[16] == 0);
	const auto fractional = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}}, {"attribute_filter", Sides({1.9, 0, 0, 0, 0, 0, 0, 0, 0})}}
	);
	CHECK(fractional.Code == Status::UnsupportedExecution);
}
TEST_CASE("Edge Detect greyscale uses remapped RGB times original alpha", "[imagegraph][source_2d]") {
	Image image = SolidImage(3, 3, {0, 0, 0, 128});
	image.Pixels[0] = 64;
	image.Pixels[1] = 128;
	image.Pixels[2] = 192;
	const auto grey = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}},
		 {"color", EnumValue{1}},
		 {"attribute_filter", Sides({1, 0, 0, 0, 0, 0, 0, 0, 0})}}
	);
	INFO(grey.Message);
	REQUIRE(grey.Ok);
	CHECK(grey.Output().Pixels[16] == 60);
	CHECK(grey.Output().Pixels[17] == 60);
	CHECK(grey.Output().Pixels[18] == 60);
	CHECK(grey.Output().Pixels[19] == 128);
	for (uint8_t alpha : {uint8_t(127), uint8_t(128)}) {
		const Image uniform = SolidImage(1, 1, {0, 0, 0, alpha});
		const auto bw = RunNode(
			"pc.edge_detect",
			{{"surface_in", &uniform}},
			{{"color", EnumValue{2}}, {"level_out", Vector2{1, 1}}}
		);
		INFO(bw.Message);
		REQUIRE(bw.Ok);
		CHECK(bw.Output().Pixels[0] == (alpha == 128 ? 255 : 0));
		CHECK(bw.Output().Pixels[3] == alpha);
	}
}
TEST_CASE(
	"Edge Detect masks mixes channels and inactive input obey common processing", "[imagegraph][source_2d]"
) {
	const Image image = Step(), white = SolidImage(1, 1, {255, 255, 255, 255}),
				black = SolidImage(1, 1, {0, 0, 0, 0});
	const auto masked = RunNode("pc.edge_detect", {{"surface_in", &image}, {"mask", &black}});
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == image.Pixels);
	const auto mixed = RunNode(
		"pc.edge_detect", {{"surface_in", &image}, {"mask", &white}}, {{"mix", .5}, {"channel", int64_t{1}}}
	);
	INFO(mixed.Message);
	REQUIRE(mixed.Ok);
	CHECK(mixed.Output().Pixels[16] == 133);
	CHECK(mixed.Output().Pixels[17] == 0);
	CHECK(mixed.Output().Pixels[19] == 192);
	const auto inactive =
		RunNode("pc.edge_detect", {{"surface_in", &image}}, {{"active", false}, {"level_in", Vector2{0, 0}}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Edge Detect diagnoses enabled source divisions and bounded switch payloads", "[imagegraph][source_2d]"
) {
	const Image image = Step();
	const auto division =
		RunNode("pc.edge_detect", {{"surface_in", &image}}, {{"level_in", Vector2{.5, .5}}});
	CHECK(division.Code == Status::UnsupportedExecution);
	CHECK(division.Port == "level_in");
	const auto shortArray = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}}, {"attribute_filter", Sides({1, 1})}}
	);
	CHECK(shortArray.Code == Status::InvalidValue);
	CHECK(shortArray.Port == "attribute_filter");
	const auto huge = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}}, {"attribute_filter", Sides({1e30, 0, 0, 0, 0, 0, 0, 0, 0})}}
	);
	CHECK(huge.Code == Status::InvalidValue);
}
TEST_CASE("Edge Detect safe grayscale draw bypasses its shader level division", "[imagegraph][source_2d]") {
	Image image;
	image.Width = image.Height = 1;
	image.Format = SurfaceFormat::R8Unorm;
	image.Pixels = {64};
	const auto run = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"level_in", Vector2{0, 0}}, {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE("Edge Detect graph preserves prior owned result under byte refusal", "[imagegraph][source_2d]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{3, 3}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{64, 128, 192, 128}}}},
		{"edge", "pc.edge_detect", "", {}, {{"level_out", Vector2{.25, .25}}}}
	};
	document.Links = {{"source", "surface_out", "edge", "surface_in"}};
	document.Outputs = {{"edges", "edge", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result;
	const auto status = Evaluate(document, plan, "edges", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(result.Pixels[0] == 64);
	CHECK(result.Pixels[3] == 128);
	const Image prior = result;
	CHECK(Evaluate(document, plan, "edges", {}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == prior);
}
TEST_CASE("Edge Detect processor rows retain independent source color modes", "[imagegraph][source_2d]") {
	Document document;
	document.FormatVersion = 9;
	Node modes{"modes", "pc.array", "", {}, {}, {}};
	modes.DynamicInputs = {{"input_0", ValueType::Scalar, 1.}, {"input_1", ValueType::Scalar, 2.}};
	document.Nodes = {
		modes,
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{0, 0, 0, 128}}}},
		{"edge", "pc.edge_detect", "", {}, {{"level_out", Vector2{1, 1}}}}
	};
	document.Links = {{"source", "surface_out", "edge", "surface_in"}, {"modes", "array", "edge", "color"}};
	document.Outputs = {{"edges", "edge", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray result;
	const auto status = EvaluateArray(document, plan, "edges", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 2);
	CHECK(result.Images[0].Pixels[0] == 128);
	CHECK(result.Images[1].Pixels[0] == 255);
	CHECK(result.Images[0].Pixels[3] == 128);
	CHECK(result.Images[1].Pixels[3] == 128);
}

TEST_CASE("Edge Detect catalogue retains the complete source neighbor default", "[imagegraph][source_2d]") {
	const auto *entry = FindCatalogueEntry("pc.edge_detect");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "attribute_filter");
	REQUIRE(input);
	CHECK(input->SourceKind == "AttributeArray");
	CHECK(input->Type == ValueType::Array);
	const auto value = CatalogueDefault(*input);
	REQUIRE(value);
	const auto *array = std::get_if<ArrayValue>(&*value);
	REQUIRE(array);
	CHECK(array->ElementType == ValueType::Scalar);
	REQUIRE(array->Elements.size() == 9);
	const std::array<double, 9> expected{1, 1, 0, 1, 0, 0, 0, 0, 0};
	for (size_t i = 0; i < expected.size(); ++i) {
		const auto *number = std::get_if<double>(&array->Elements[i]);
		REQUIRE(number);
		CHECK(*number == expected[i]);
	}
	Image image = SolidImage(3, 3, {0, 0, 0, 255});
	image.Pixels[0] = 64;
	const auto defaults = RunNode("pc.edge_detect", {{"surface_in", &image}}, {{"algorithm", EnumValue{3}}});
	INFO(defaults.Message);
	REQUIRE(defaults.Ok);
	CHECK(defaults.Output().Pixels[16] == 64);
	const auto explicitControls = RunNode(
		"pc.edge_detect",
		{{"surface_in", &image}},
		{{"algorithm", EnumValue{3}}, {"attribute_filter", Sides({1, 1, 0, 1, 0, 0, 0, 0, 0})}}
	);
	REQUIRE(explicitControls.Ok);
	CHECK(defaults.Output().Pixels == explicitControls.Output().Pixels);
}

TEST_CASE(
	"Edge Detect preserves flat boolean integer and mixed source switch arrays", "[imagegraph][source_2d]"
) {
	Image image = SolidImage(3, 3, {0, 0, 0, 255});
	image.Pixels[0] = 64;
	for (ValueType type : {ValueType::Boolean, ValueType::Integer, ValueType::Any}) {
		ArrayValue switches;
		switches.ElementType = type;
		for (size_t i = 0; i < 9; ++i) {
			if (type == ValueType::Boolean)
				switches.Elements.emplace_back(i == 0);
			else if (type == ValueType::Integer)
				switches.Elements.emplace_back(int64_t(i == 0));
			else
				switches.Items.push_back(
					{i == 0	  ? ElementValue{true}
					 : i == 1 ? ElementValue{int64_t{0}}
							  : ElementValue{0.}}
				);
		}
		const auto run = RunNode(
			"pc.edge_detect",
			{{"surface_in", &image}},
			{{"algorithm", EnumValue{3}}, {"attribute_filter", switches}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[16] == 64);
		if (type == ValueType::Any) {
			switches.Items[0].Data = std::vector<SourceArrayItem>{{ElementValue{true}}};
			const auto nested = RunNode(
				"pc.edge_detect",
				{{"surface_in", &image}},
				{{"algorithm", EnumValue{3}}, {"attribute_filter", switches}}
			);
			CHECK(nested.Code == Status::InvalidValue);
			CHECK(nested.Port == "attribute_filter");
		}
	}
}

TEST_CASE("EdgeDetect admits total processor-row work before output allocation", "[imagegraph][source_2d]") {
	const auto *entry = FindCatalogueEntry("pc.edge_detect");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.edge_detect");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"bounded", "pc.edge_detect", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 65536;
	const Image image = SolidImage(32, 32, {64, 128, 192, 128});
	context.Images.emplace_back("surface_in", &image);
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_in");
	CHECK(context.OutputImages.empty());
}
