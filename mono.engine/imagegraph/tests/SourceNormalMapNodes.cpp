#include "../src/nodes/Curve.hpp"
#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_normal_maps")
using namespace engine::imagegraph;
namespace {
	Document NormalFixture(std::string operation = "pc.normal_blend") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"base",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}},
			{"second",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 255, 255}}}},
			{"node", operation, "", {}, {{"attribute_color_depth", EnumValue{5}}}}
		};
		document.Links = {
			{"base",
			 "image",
			 "node",
			 operation == "pc.normal_blend"	  ? "normal_1"
			 : operation == "pc.normal_light" ? "surface_in"
											  : "normal_in"}
		};
		if (operation == "pc.normal_blend") {
			document.Links.push_back({"second", "image", "node", "surface_2"});
			document.Nodes.back().Values.push_back({"normalize", false});
		}
		document.Outputs = {{"image", "node", "surface_out"}};
		return document;
	}
	void NormalSet(Node &node, std::string port, Value value) {
		for (auto &input : node.DynamicInputs)
			if (input.Id == port) {
				input.Default = std::move(value);
				return;
			}
		for (auto &entry : node.Values)
			if (entry.Port == port) {
				entry.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::move(port), std::move(value)});
	}
	Image NormalEvaluate(Document &document, std::string output = "image") {
		Plan plan;
		Diagnostic diagnostic;
		const Status compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const Status evaluated = Evaluate(document, plan, output, image, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return image;
	}
	void NormalPixel(const Image &image, detail::Rgba expected, double tolerance = 1e-6) {
		const auto actual = detail::ReadPixel(image, 0, 0);
		for (size_t i = 0; i < 4; i++)
			REQUIRE(actual[i] == Catch::Approx(expected[i]).margin(tolerance));
	}
	void NormalLightGroup(Node &node, size_t group = 0) {
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		for (const auto &input : entry->DynamicTemplate)
			node.DynamicInputs.push_back(
				{std::string(input.Id) + "_" + std::to_string(group), input.Type, CatalogueDefault(input)}
			);
		NormalSet(node, "intensity_" + std::to_string(group), 1.0);
		NormalSet(node, "range_" + std::to_string(group), 4.0);
		NormalSet(node, "position_" + std::to_string(group), Vector2{.5, .5});
		NormalSet(node, "distance_" + std::to_string(group), 100.0);
		NormalSet(node, "color_" + std::to_string(group), Colour{255, 0, 0, 255});
	}
}
TEST_CASE(
	"Normal blend keeps packed normalization mask quirk and source enum sentinel modes",
	"[source_normal_maps]"
) {
	auto document = NormalFixture();
	const std::array<int64_t, 5> modes{0, 1, 3, 4, 6};
	const std::array<detail::Rgba, 5> colours{
		{{.5, .5, 1, 1}, {1, 1, 1, 1}, {1.5, 1.5, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}}
	};
	for (size_t i = 0; i < modes.size(); i++) {
		NormalSet(document.Nodes.back(), "blend_mode", EnumValue{modes[i]});
		NormalPixel(NormalEvaluate(document), colours[i]);
	}
	NormalSet(document.Nodes.back(), "blend_mode", EnumValue{0});
	NormalSet(document.Nodes.back(), "intensity", .5);
	NormalPixel(NormalEvaluate(document), {.75, .75, 1, 1});
	NormalSet(document.Nodes.back(), "intensity", 1.0);
	NormalSet(document.Nodes.back(), "normalize", true);
	const double length = std::sqrt(1.5);
	NormalPixel(NormalEvaluate(document), {.5 / length, .5 / length, 1 / length, 1});
	NormalSet(document.Nodes.back(), "normalize", false);
	document.Nodes.push_back(
		{"mask",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 255}}}}
	);
	document.Links.push_back({"mask", "image", "node", "mask"});
	NormalPixel(NormalEvaluate(document), {.5, .5, 0, 1});
	NormalSet(document.Nodes[2], "mask_alpha_only", true);
	// This node's custom shader does not read the generic Mask Alpha Only attribute.
	NormalPixel(NormalEvaluate(document), {.5, .5, 0, 1});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(NormalEvaluate(restored) == NormalEvaluate(document));
}
TEST_CASE(
	"Normal blend transforms both inputs with nearest clamp and preserves alpha on absent foreground",
	"[source_normal_maps]"
) {
	const Image first = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 128, 0, 255, 0, 255});
	const auto transformed =
		imagegraph_test::RunNode("pc.normal_blend", {{"normal_1", &first}}, {{"rotation_1", 180.0}});
	INFO(transformed.Message);
	REQUIRE(transformed.Ok);
	NormalPixel(transformed.Output(), {0, 1, 0, 1});
	REQUIRE(detail::ReadPixel(transformed.Output(), 1, 0) == detail::Rgba{1, 0, 0, 128.0 / 255});
	const auto second = imagegraph_test::MakeImage(2, 1, {0, 0, 255, 255, 255, 255, 255, 255});
	const auto shifted = imagegraph_test::RunNode(
		"pc.normal_blend",
		{{"normal_1", &first}, {"surface_2", &second}},
		{{"normalize", false},
		 {"blend_mode", EnumValue{6}},
		 {"position_2", Vector2{1, 0}},
		 {"position_2_unit", EnumValue{0}},
		 {"anchor_2", Vector2{0, 0}},
		 {"scale_2", Vector2{-1, 1}},
		 {"rotation_2", 180.0}}
	);
	INFO(shifted.Message);
	REQUIRE(shifted.Ok);
	NormalPixel(shifted.Output(), {0, 0, 1, 1});
	REQUIRE(first.Pixels == std::vector<uint8_t>{255, 0, 0, 128, 0, 255, 0, 255});
	const auto zero =
		imagegraph_test::RunNode("pc.normal_blend", {{"normal_1", &first}}, {{"scale_1", Vector2{0, 1}}});
	REQUIRE_FALSE(zero.Ok);
	REQUIRE(zero.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Normal height integrates selected sweeps through R16 and persists control arrays", "[source_normal_maps]"
) {
	auto document = NormalFixture("pc.normal_to_height");
	NormalSet(document.Nodes.front(), "colour", Colour{255, 0, 255, 255});
	NormalSet(document.Nodes.back(), "max_itr", int64_t{1});
	const std::array<double, 16> heights{
		0,
		.5,
		-.5,
		0,
		-.5,
		0,
		-.5,
		-.1666259765625,
		.5,
		.5,
		0,
		.1666259765625,
		0,
		.1666259765625,
		-.1666259765625,
		0
	};
	for (int64_t direction = 0; direction < 16; direction++) {
		NormalSet(document.Nodes.back(), "sweep_direction", direction);
		NormalPixel(
			NormalEvaluate(document),
			{heights[size_t(direction)], heights[size_t(direction)], heights[size_t(direction)], 1}
		);
	}
	NormalSet(document.Nodes.back(), "sweep_direction", int64_t{1});
	NormalSet(document.Nodes.back(), "max_itr", int64_t{3});
	NormalPixel(NormalEvaluate(document), {1.5, 1.5, 1.5, 1});
	NormalSet(document.Nodes.back(), "max_itr", int64_t{0});
	NormalSet(document.Nodes.back(), "base_height", .5);
	NormalPixel(NormalEvaluate(document), {.501953125, .501953125, .501953125, 1});
	NormalSet(
		document.Nodes.back(), "max_itr", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{2}}}
	);
	NormalSet(document.Nodes.back(), "base_height", 0.0);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	const Status arrayStatus = EvaluateArray(document, plan, "image", {}, images, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(arrayStatus == Status::Ok);
	REQUIRE(images.Images.size() == 3);
	REQUIRE(images.Items.size() == 3);
	for (size_t i = 0; i < 3; i++)
		NormalPixel(images.Images[i], {i * .5, i * .5, i * .5, 1});
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, plan, "image", {}, replay, diagnostic) == Status::Ok);
	REQUIRE(replay.Images == images.Images);
	NormalSet(document.Nodes.back(), "max_itr", int64_t{100'000'000});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image sentinel = imagegraph_test::MakeImage(1, 1, {1, 2, 3, 4});
	const Image original = sentinel;
	REQUIRE(Evaluate(document, plan, "image", sentinel, diagnostic) == Status::LimitExceeded);
	REQUIRE(sentinel == original);
	NormalSet(document.Nodes.back(), "max_itr", int64_t{1});
	NormalSet(document.Nodes.back(), "normal_height", 0.0);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "image", sentinel, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(sentinel == original);
}
TEST_CASE(
	"Normal light renders four complete light groups and source attenuation choices", "[source_normal_maps]"
) {
	auto document = NormalFixture("pc.normal_light");
	NormalSet(document.Nodes.front(), "colour", Colour{0, 0, 0, 128});
	NormalLightGroup(document.Nodes.back());
	document.Outputs.push_back({"light", "node", "light_only"});
	auto &node = document.Nodes.back();
	NormalSet(node, "type_0", EnumValue{0});
	NormalPixel(NormalEvaluate(document, "light"), {143.0 / 255, 0, 0, 1});
	NormalSet(node, "attenuation_0", EnumValue{1});
	NormalPixel(NormalEvaluate(document, "light"), {239.0 / 255, 0, 0, 1});
	NormalSet(node, "attenuation_0", EnumValue{2});
	NormalPixel(NormalEvaluate(document, "light"), {191.0 / 255, 0, 0, 1});
	Curve custom;
	custom.Header = {0, 1, 0, .25, .75, 1};
	custom.Anchors = {{{0, 0, 0, 0, 0, 0}}, {{0, 0, 1, 1, 0, 0}}};
	NormalSet(node, "attenuation_0", EnumValue{3});
	NormalSet(node, "atten_curve_0", custom);
	NormalPixel(NormalEvaluate(document, "light"), {159.0 / 255, 0, 0, 1});
	Curve nonlinear;
	nonlinear.Header = {0, 1, 0, 0, 1, 1};
	nonlinear.Anchors = {{{0, 0, 0, 0, 1.0 / 3, 2.0 / 3}}, {{-1.0 / 3, 0, 1, 1, 0, 0}}};
	NormalSet(node, "atten_curve_0", nonlinear);
	NormalPixel(NormalEvaluate(document, "light"), {239.0 / 255, 0, 0, 1});
	Curve stepped;
	stepped.Header = {0, 1, 1, 0, 1, 1};
	stepped.Anchors = {{{0, 0, 0, 0, 0, 0}}, {{0, 0, .5, .4, 0, 0}}, {{0, 0, 1, 1, 0, 0}}};
	NormalSet(node, "atten_curve_0", stepped);
	NormalPixel(NormalEvaluate(document, "light"), {102.0 / 255, 0, 0, 1});
	custom.Header[1] = 0;
	NormalSet(node, "atten_curve_0", custom);
	// Positive brightness divided by zero becomes +Inf, then the shader clamps to its last anchor.
	NormalPixel(NormalEvaluate(document, "light"), {191.0 / 255, 0, 0, 1});

	NormalSet(node, "type_0", EnumValue{1});
	NormalPixel(NormalEvaluate(document, "light"), {1, 0, 0, 1});
	NormalSet(node, "type_0", EnumValue{2});
	NormalSet(node, "attenuation_0", EnumValue{0});
	NormalSet(node, "position_0", Vector2{0, .5});
	NormalSet(node, "end_position_0", Vector2{1, .5});
	NormalSet(node, "end_distance_0", 100.0);
	NormalSet(node, "end_color_0", Colour{0, 0, 255, 255});
	NormalPixel(NormalEvaluate(document, "light"), {72.0 / 255, 0, 72.0 / 255, 1});
	NormalSet(node, "type_0", EnumValue{3});
	NormalSet(node, "position_0", Vector2{.5, .5});
	NormalSet(node, "end_position_0", Vector2{.5, .5});
	NormalSet(node, "end_distance_0", 0.0);
	NormalPixel(NormalEvaluate(document, "light"), {1, 0, 0, 1});
	NormalPixel(NormalEvaluate(document), {1, 0, 0, 128.0 / 255});
	NormalSet(node, "keep_alpha", false);
	NormalPixel(NormalEvaluate(document), {1, 0, 0, 1});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(NormalEvaluate(restored) == NormalEvaluate(document));
}
TEST_CASE(
	"Normal light banding height normals ambient and ordered saturation retain source semantics",
	"[source_normal_maps]"
) {
	auto document = NormalFixture("pc.normal_light");
	NormalLightGroup(document.Nodes.back());
	auto &node = document.Nodes.back();
	NormalSet(document.Nodes.front(), "colour", Colour{0, 255, 0, 128});
	NormalSet(node, "ambient", Colour{0, 255, 0, 255});
	NormalSet(node, "type_0", EnumValue{0});
	NormalSet(node, "position_0", Vector2{0, 0});
	NormalSet(node, "radial_banding_0", int64_t{2});
	NormalSet(node, "radial_shadow_0", .5);
	NormalSet(node, "attenuation_0", EnumValue{2});
	NormalSet(node, "banding_0", int64_t{2});
	NormalPixel(NormalEvaluate(document), {128.0 / 255, 1, 0, 128.0 / 255});
	NormalSet(node, "radial_start_0", 90.0);
	NormalPixel(NormalEvaluate(document), {1, 1, 0, 128.0 / 255});
	NormalSet(node, "radial_band_ratio_0", 1.0);
	NormalPixel(NormalEvaluate(document), {128.0 / 255, 1, 0, 128.0 / 255});
	NormalLightGroup(node, 1);
	NormalSet(node, "type_1", EnumValue{1});
	NormalSet(node, "intensity_1", .75);
	NormalPixel(NormalEvaluate(document), {1, 1, 0, 128.0 / 255});
	// Height and normal texture reads are exercised on the actual linked graph.
	document.Nodes[1].Values.back() = {"colour", Colour{0, 0, 255, 255}};
	document.Links.push_back({"second", "image", "node", "normal_map"});
	document.Links.push_back({"second", "image", "node", "height_map"});
	NormalSet(document.Nodes[2], "height", .25);
	NormalSet(document.Nodes[2], "position_0", Vector2{.5, .5});
	NormalSet(document.Nodes[2], "radial_banding_0", int64_t{0});
	NormalSet(document.Nodes[2], "banding_0", int64_t{0});
	NormalSet(document.Nodes[2], "intensity_1", 0.0);
	const Image lit = NormalEvaluate(document);
	// Blue decodes to (1,1,-1); height raises the sample by 1/12 before point attenuation.
	NormalPixel(lit, {107.0 / 255, 1, 0, 128.0 / 255});
	NormalSet(document.Nodes[2], "height", 0.0);
	NormalPixel(NormalEvaluate(document), {110.0 / 255, 1, 0, 128.0 / 255});
	NormalSet(document.Nodes[2], "range_0", 0.0);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image preserved = lit;
	const Status refusal = Evaluate(document, plan, "image", preserved, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(refusal == Status::UnsupportedExecution);
	REQUIRE(preserved == lit);
}
