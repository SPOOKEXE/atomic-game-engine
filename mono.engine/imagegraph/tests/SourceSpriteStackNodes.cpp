#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>

TEST_SUITE_ID("engine.imagegraph.source_sprite_stack")
using namespace engine::imagegraph;
namespace {
	Document SpriteStackFixture(int64_t amount = 2) {
		Document document;
		document.FormatVersion = 9;
		document.Project.emplace().SurfaceWidth = 4;
		document.Project->SurfaceHeight = 1;
		document.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}},
			{"stack",
			 "pc.sprite_stack",
			 "",
			 {},
			 {{"dimension", Vector2{4, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"position_unit", EnumValue{0}},
			  {"stack_amount", amount},
			  {"stack_shift", Vector2{1, 0}},
			  {"alpha_end", .5},
			  {"stack_blend", Colour{0, 0, 255, 255}},
			  {"attribute_color_depth", EnumValue{5}}}}
		};
		document.Links = {{"source", "image", "stack", "base_shape"}};
		document.Outputs = {{"image", "stack", "surface_out"}};
		return document;
	}
	Image SpriteStackEvaluate(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		auto evaluated = Evaluate(document, plan, "image", image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return image;
	}

	void SpriteStackPixel(const Image &image, uint32_t x, uint32_t y, detail::Rgba expected) {
		const auto actual = detail::ReadPixel(image, x, y);
		INFO("pixel " << x << "," << y);
		for (size_t channel = 0; channel < 4; channel++) {
			INFO("channel " << channel);
			REQUIRE(actual[channel] == Catch::Approx(expected[channel]).margin(1e-6));
		}
	}
}
TEST_CASE(
	"Sprite Stack preserves ordered copies extra white base and final normal blend", "[source_sprite_stack]"
) {
	auto document = SpriteStackFixture();
	const Image image = SpriteStackEvaluate(document);
	SpriteStackPixel(image, 0, 0, {1, 1, 1, 1});
	SpriteStackPixel(image, 1, 0, {0, 0, .75, .5625});
	SpriteStackPixel(image, 2, 0, {0, 0, .5, .25});
	SpriteStackPixel(image, 3, 0, {0, 0, 0, 0});
	const std::string saved = Write(document);
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(saved, restored, diagnostic) == Status::Ok);
	REQUIRE(SpriteStackEvaluate(restored) == image);
	document.Nodes.back().Values.push_back({"move_base", true});
	const Image moved = SpriteStackEvaluate(document);
	SpriteStackPixel(moved, 0, 0, {0, 0, .5, .25});
	SpriteStackPixel(moved, 1, 0, {0, 0, 0, 0});
	const Image noCopies = SpriteStackEvaluate(SpriteStackFixture(0));
	SpriteStackPixel(noCopies, 0, 0, {.5, .5, .5, .25});
}
TEST_CASE(
	"Sprite Stack keeps source dimension modes and raw Dimension position reference", "[source_sprite_stack]"
) {
	auto document = SpriteStackFixture(0);
	auto &inputs = document.Nodes.back().Values;
	inputs.push_back({"output_dimension_type", EnumValue{2}});
	inputs.push_back({"relative_dimension", Vector2{3, 2}});
	Image relative = SpriteStackEvaluate(document);
	REQUIRE(relative.Width == 3);
	REQUIRE(relative.Height == 2);
	inputs.back() = {"relative_dimension", Vector2{20, 20}};
	inputs[inputs.size() - 2] = {"output_dimension_type", EnumValue{3}};
	Image fit = SpriteStackEvaluate(document);
	REQUIRE(fit.Width == 1);
	REQUIRE(fit.Height == 1);
	inputs[inputs.size() - 2] = {"output_dimension_type", EnumValue{0}};
	inputs.push_back({"position", Vector2{.5, 0}});
	for (auto &input : inputs)
		if (input.Port == "position_unit") input.Data = EnumValue{1};
	const Image displaced = SpriteStackEvaluate(document);
	// Source getDimension references the authored 4x1 Dimension, not the 1x1 output.
	SpriteStackPixel(displaced, 0, 0, {0, 0, 0, 0});
}
TEST_CASE(
	"Sprite Stack highlight ignores Highlight Alpha and keeps source undefined boundaries",
	"[source_sprite_stack]"
) {
	auto document = SpriteStackFixture(1);
	auto &inputs = document.Nodes.back().Values;
	inputs.push_back({"highlight", EnumValue{1}});
	inputs.push_back({"highlight_color", Colour{255, 0, 0, 128}});
	inputs.push_back({"highlight_alpha", 0.0});
	const Image colour = SpriteStackEvaluate(document);
	SpriteStackPixel(colour, 1, 0, {1, 127.0 / 255, 127.0 / 255, 1});
	inputs.back().Data = 1.0;
	REQUIRE(SpriteStackEvaluate(document) == colour);
	inputs[inputs.size() - 3].Data = EnumValue{2};
	for (auto &input : inputs)
		if (input.Port == "stack_shift") input.Data = Vector2{0, 0};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image prior = colour;
	REQUIRE(Evaluate(document, plan, "image", prior, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(prior == colour);
	inputs[inputs.size() - 3].Data = EnumValue{0};
	for (auto &input : inputs)
		if (input.Port == "stack_amount") input.Data = int64_t{100'000'000};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "image", prior, diagnostic) == Status::LimitExceeded);
	REQUIRE(prior == colour);
}
TEST_CASE(
	"Sprite Stack combined surface arrays preserve order while individual mode uses processor rows",
	"[source_sprite_stack]"
) {
	auto document = SpriteStackFixture(1);
	document.Nodes[0].Values.back() = {"colour", Colour{255, 0, 0, 255}};
	document.Nodes.push_back(
		{"green",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 255, 0, 255}}}}
	);
	Node collector{"array", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	collector.DynamicInputs = {
		{"input_0", ValueType::Image, std::nullopt}, {"input_1", ValueType::Image, std::nullopt}
	};
	document.Nodes.push_back(std::move(collector));
	document.Links = {
		{"source", "image", "array", "input_0"},
		{"green", "image", "array", "input_1"},
		{"array", "array", "stack", "base_shape"}
	};
	auto &inputs = document.Nodes[1].Values;
	for (auto &input : inputs)
		if (input.Port == "stack_blend") input.Data = Colour{255, 255, 255, 255};
	inputs.push_back({"highlight", EnumValue{2}});
	inputs.push_back({"highlight_alpha", 0.0});
	const Image combined = SpriteStackEvaluate(document);
	// Combined source mode ignores authored amount, alpha_end and all highlight controls.
	SpriteStackPixel(combined, 0, 0, {1, 0, 0, 1});
	SpriteStackPixel(combined, 1, 0, {0, 1, 0, 1});
	SpriteStackPixel(combined, 2, 0, {0, 0, 0, 0});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(SpriteStackEvaluate(restored) == combined);
	inputs[inputs.size() - 2].Data = EnumValue{0};
	inputs.push_back({"array_process", EnumValue{0}});
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray individual;
	REQUIRE(EvaluateArray(document, plan, "image", {}, individual, diagnostic) == Status::Ok);
	REQUIRE(individual.Items.size() == 2);
	REQUIRE(individual.Images.size() == 2);
	SpriteStackPixel(individual.Images[0], 0, 0, {1, 0, 0, 1});
	SpriteStackPixel(individual.Images[0], 1, 0, {.5, 0, 0, .25});
	SpriteStackPixel(individual.Images[1], 0, 0, {0, 1, 0, 1});
	SpriteStackPixel(individual.Images[1], 1, 0, {0, .5, 0, .25});
}
TEST_CASE(
	"Sprite Stack rotation uses the source centre pivot and preserves input pixels", "[source_sprite_stack]"
) {
	const Image source = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255});
	const auto rotated = imagegraph_test::RunNode(
		"pc.sprite_stack",
		{{"base_shape", &source}},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"position_unit", EnumValue{0}},
		 {"stack_amount", int64_t{0}},
		 {"rotation", 180.0},
		 {"alpha_end", 1.0}}
	);
	INFO(rotated.Message);
	REQUIRE(rotated.Ok);
	SpriteStackPixel(rotated.Output(), 0, 0, {0, 1, 0, 1});
	SpriteStackPixel(rotated.Output(), 1, 0, {1, 0, 0, 1});
	REQUIRE(source.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255});
}
