#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <tuple>
TEST_SUITE_ID("engine.imagegraph.source_ordered_dither")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Solid(uint32_t width, uint32_t height, Colour colour = {128, 128, 128, 255}) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (size_t p = 0; p < size_t(width) * height; ++p) {
			image.Pixels[p * 4] = colour.Red;
			image.Pixels[p * 4 + 1] = colour.Green;
			image.Pixels[p * 4 + 2] = colour.Blue;
			image.Pixels[p * 4 + 3] = colour.Alpha;
		}
		return image;
	}
	Document Graph(int64_t type = 0) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{2, 2}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{128, 128, 128, 255}}}},
			{"dither",
			 "pc.dither",
			 "",
			 {},
			 {{"mode", EnumValue{0}},
			  {"color_type", EnumValue{type}},
			  {"steps", int64_t{1}},
			  {"r_steps", int64_t{1}},
			  {"g_steps", int64_t{1}},
			  {"b_steps", int64_t{1}},
			  {"seed", 23.0}}}
		};
		document.Links = {{"source", "surface_out", "dither", "surface_in"}};
		document.Outputs = {{"out", "dither", "surface_out"}};
		return document;
	}
	void CheckRows(const Image &image, std::initializer_list<std::string_view> rows) {
		REQUIRE(image.Height == rows.size());
		size_t y = 0;
		for (auto row : rows) {
			REQUIRE(row.size() == image.Width);
			for (size_t x = 0; x < row.size(); ++x)
				for (size_t c = 0; c < 3; ++c)
					CHECK(image.Pixels[(y * image.Width + x) * 4 + c] == (row[x] == '#' ? 255 : 0));
			++y;
		}
	}
}
TEST_CASE(
	"Ordered Dither source Bayer tables retain 2 4 and 8 pixel goldens",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	for (int64_t pattern = 0; pattern < 3; ++pattern) {
		const uint32_t side = 2u << pattern;
		const auto input = Solid(side, side);
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}},
			{{"pattern", EnumValue{pattern}}, {"color_type", EnumValue{0}}, {"steps", int64_t{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		if (pattern == 0) CheckRows(run.Output(), {"#.", ".#"});
		if (pattern == 1) CheckRows(run.Output(), {"###.", ".#.#", "#.#.", ".#.#"});
		if (pattern == 2)
			CheckRows(
				run.Output(),
				{"###.#.#.",
				 ".#.#.#.#",
				 "#.#.#.#.",
				 ".#.#.#.#",
				 "#.#.###.",
				 ".#.#.#.#",
				 "#.#.#.#.",
				 ".#.#.#.#"}
			);
	}
}
TEST_CASE(
	"Ordered Dither palette orders its nearest two source Lab colours",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	auto run = RunNode("pc.dither", {{"surface_in", &input}});
	REQUIRE(run.Ok);
	CheckRows(run.Output(), {".#", "#."});
}
TEST_CASE(
	"Ordered Dither RGB HSV and greyscale quantizers preserve source alpha square",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2, {128, 128, 128, 128});
	for (int64_t type : {0, 2, 3}) {
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}},
			{{"color_type", EnumValue{type}},
			 {"steps", int64_t{1}},
			 {"r_steps", int64_t{1}},
			 {"g_steps", int64_t{1}},
			 {"b_steps", int64_t{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CheckRows(run.Output(), {"#.", ".#"});
		for (size_t p = 0; p < 4; ++p)
			CHECK(run.Output().Pixels[p * 4 + 3] == 64);
	}
}
TEST_CASE(
	"Ordered Dither palette alpha multiplies source alpha after colour selection",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2, {128, 128, 128, 128});
	ArrayValue palette{ValueType::Colour, {Colour{255, 255, 255, 128}, Colour{0, 0, 0, 64}}};
	auto run = RunNode("pc.dither", {{"surface_in", &input}}, {{"palette", palette}});
	REQUIRE(run.Ok);
	CheckRows(run.Output(), {".#", "#."});
	CHECK(run.Output().Pixels[3] == 32);
	CHECK(run.Output().Pixels[7] == 64);
	CHECK(run.Output().Pixels[11] == 64);
	CHECK(run.Output().Pixels[15] == 32);
}
TEST_CASE(
	"Ordered Dither exact colour bypasses pattern and still squares alpha",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(1, 1, {64, 64, 64, 128});
	auto run =
		RunNode("pc.dither", {{"surface_in", &input}}, {{"color_type", EnumValue{0}}, {"steps", int64_t{4}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 64});
}
TEST_CASE(
	"Ordered Dither Linear uses raw Bayer ranks and inversion",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	for (bool invert : {false, true}) {
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}},
			{{"color_type", EnumValue{0}},
			 {"steps", int64_t{1}},
			 {"map_mode", EnumValue{1}},
			 {"invert", invert}}
		);
		REQUIRE(run.Ok);
		CheckRows(
			run.Output(),
			invert ? std::initializer_list<std::string_view>{"##", "##"}
				   : std::initializer_list<std::string_view>{"..", ".."}
		);
	}
}
TEST_CASE(
	"Ordered Dither Matrix keeps literal numeric thresholds", "[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"pattern", EnumValue{5}},
		 {"dither_matrix", MatrixValue{2, 2, {0, 2, 3, 1}}}}
	);
	REQUIRE(run.Ok);
	CheckRows(run.Output(), {"#.", ".#"});
	auto zero = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"pattern", EnumValue{5}},
		 {"dither_matrix", MatrixValue{2, 2, {0, 0, 0, 0}}}}
	);
	REQUIRE(zero.Ok);
	CheckRows(zero.Output(), {"##", "##"});
}
TEST_CASE(
	"Ordered Dither Custom luminance samples repeat in pixel space",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	const auto map =
		imagegraph_test::MakeImage(2, 2, {0, 0, 0, 1, 255, 255, 255, 1, 128, 128, 128, 1, 64, 64, 64, 1});
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}, {"dither_map", &map}},
		{{"pattern", EnumValue{4}}, {"color_type", EnumValue{0}}, {"steps", int64_t{1}}}
	);
	REQUIRE(run.Ok);
	CheckRows(run.Output(), {"#.", "##"});
}
TEST_CASE(
	"Ordered Dither spatial scale snaps sampling and Bayer positions together",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = imagegraph_test::MakeImage(
		4, 1, {32, 32, 32, 255, 96, 96, 96, 255, 192, 192, 192, 255, 192, 192, 192, 255}
	);
	auto base =
		RunNode("pc.dither", {{"surface_in", &input}}, {{"color_type", EnumValue{0}}, {"steps", int64_t{1}}});
	auto scaled = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}}, {"steps", int64_t{1}}, {"dither_scale", Vector2{2, 1}}}
	);
	REQUIRE(base.Ok);
	REQUIRE(scaled.Ok);
	CheckRows(base.Output(), {"#.##"});
	CheckRows(scaled.Output(), {"####"});
}
TEST_CASE(
	"Ordered Dither mapped contrast missing surface uses source range x",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2, {64, 64, 64, 255});
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"contrast", 9.0},
		 {"contrast_mapped", true},
		 {"contrast_map_range", Vector2{0, 0}}}
	);
	REQUIRE(run.Ok);
	CheckRows(run.Output(), {"#.", ".#"});
	auto unmarked = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"contrast", 1.0},
		 {"contrast_mapped", false},
		 {"contrast_map_range", Vector2{0, 0}}}
	);
	REQUIRE(unmarked.Ok);
	CheckRows(unmarked.Output(), {"#.", ".."});
}
TEST_CASE(
	"Ordered Dither mapped contrast reads mean RGB at output coordinates",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2), map = Solid(2, 2, {0, 255, 0, 1}),
			   white = Solid(2, 2, {255, 255, 255, 1});
	for (const auto *surface : {&map, &white}) {
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}, {"contrast_map", surface}},
			{{"color_type", EnumValue{0}},
			 {"steps", int64_t{1}},
			 {"contrast_mapped", true},
			 {"contrast_map_range", Vector2{0, 10}}}
		);
		REQUIRE(run.Ok);
		CheckRows(
			run.Output(),
			surface == &map ? std::initializer_list<std::string_view>{"#.", ".#"}
							: std::initializer_list<std::string_view>{"##", ".#"}
		);
	}
}

TEST_CASE(
	"Ordered Dither inert noise inversion retains seed repeatability",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}}, {"steps", int64_t{1}}, {"pattern", EnumValue{3}}, {"seed", 23.0}}
	);
	auto invert = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"pattern", EnumValue{3}},
		 {"seed", 23.0},
		 {"invert", true}}
	);
	REQUIRE(run.Ok);
	REQUIRE(invert.Ok);
	CHECK(run.Output().Pixels == invert.Output().Pixels);
	CheckRows(run.Output(), {".#", "##"});
}
TEST_CASE(
	"Ordered Dither ambient and undefined branches remain explicit",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	for (const auto &[port, value] : std::array<std::pair<std::string_view, Value>, 4>{
			 {{"mode", EnumValue{1}},
			  {"pattern", EnumValue{4}},
			  {"dither_scale", Vector2{0, 1}},
			  {"steps", int64_t{0}}}
		 }) {
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}},
			{{"color_type", EnumValue{0}}, {"steps", int64_t{port == "steps" ? 0 : 1}}, {port, value}}
		);
		INFO(port);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
	}
	const auto map = Solid(2, 2);
	auto linear = RunNode(
		"pc.dither",
		{{"surface_in", &input}, {"dither_map", &map}},
		{{"pattern", EnumValue{4}}, {"map_mode", EnumValue{1}}}
	);
	CHECK_FALSE(linear.Ok);
	CHECK(linear.Code == Status::UnsupportedExecution);
	auto escaped = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}}, {"steps", int64_t{1}}, {"map_mode", EnumValue{1}}, {"contrast", 100.0}}
	);
	CHECK_FALSE(escaped.Ok);
	CHECK(escaped.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Ordered Dither source shader slot bounds and empty palette refuse",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	auto large = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"pattern", EnumValue{5}}, {"dither_matrix", MatrixValue{9, 8, std::vector<double>(72)}}}
	);
	CHECK_FALSE(large.Ok);
	CHECK(large.Code == Status::LimitExceeded);
	auto empty =
		RunNode("pc.dither", {{"surface_in", &input}}, {{"palette", ArrayValue{ValueType::Colour, {}}}});
	CHECK_FALSE(empty.Ok);
	CHECK(empty.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Ordered Dither red-only source safe draw bypasses its selected shader",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	Image input{1, 1, {64}, 0};
	input.Format = SurfaceFormat::R8Unorm;
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"mode", EnumValue{1}}, {"pattern", EnumValue{4}}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Ordered Dither common processor controls preserve original image",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	for (const auto &[port, value] : std::array<std::pair<std::string_view, Value>, 3>{
			 {{"active", false}, {"mix", 0.0}, {"channel", int64_t{0}}}
		 }) {
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}},
			{{"color_type", EnumValue{0}}, {"steps", int64_t{1}}, {port, value}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == input.Pixels);
	}
}
TEST_CASE(
	"Ordered Dither actual graphs persist all four colour choices",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	for (int64_t type = 0; type < 4; ++type) {
		auto document = Graph(type);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		Image result;
		auto status = Evaluate(restored, plan, "out", {}, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CheckRows(
			result,
			type == 1 ? std::initializer_list<std::string_view>{".#", "#."}
					  : std::initializer_list<std::string_view>{"#.", ".#"}
		);
	}
}
TEST_CASE(
	"Ordered Dither actual processor colour rows match scalar graph oracles",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	auto document = Graph();
	Node types{"types", "pc.array", "", {}, {}, {}};
	for (size_t i = 0; i < 4; ++i)
		types.DynamicInputs.push_back({"input_" + std::to_string(i), ValueType::Scalar, double(i)});
	document.Nodes.push_back(std::move(types));
	document.Links.push_back({"types", "array", "dither", "color_type"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray result;
	auto status = EvaluateArray(document, plan, "out", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 4);
	for (size_t i = 0; i < 4; ++i) {
		auto scalar = Graph(int64_t(i));
		Plan compiled;
		REQUIRE(Compile(scalar, compiled, diagnostic) == Status::Ok);
		Image oracle;
		REQUIRE(Evaluate(scalar, compiled, "out", {}, oracle, diagnostic) == Status::Ok);
		CHECK(result.Images[i] == oracle);
	}
}
TEST_CASE(
	"Ordered Dither actual later feather work refusal wins before first output byte refusal",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	auto document = Graph();
	document.Nodes[0].Values[0].Data = Vector2{256, 256};
	Node feathers{"feathers", "pc.array", "", {}, {}, {}};
	feathers.DynamicInputs = {{"input_0", ValueType::Scalar, 0.0}, {"input_1", ValueType::Scalar, 64.0}};
	document.Nodes.push_back(std::move(feathers));
	document.Links.push_back({"feathers", "array", "dither", "mask_feather"});
	document.Links.push_back({"source", "surface_out", "dither", "mask"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result = Solid(1, 1, {13, 14, 15, 16});
	const Image prior = result;
	auto status = Evaluate(document, plan, "out", {}, result, diagnostic, 500000);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "dither");
	CHECK(diagnostic.Message.find("work budget") != std::string::npos);
	CHECK(result == prior);
}
TEST_CASE(
	"Ordered Dither output byte failure preserves prior public pixels",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result = Solid(1, 1, {13, 14, 15, 16});
	const Image prior = result;
	CHECK(Evaluate(document, plan, "out", {}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == prior);
}
TEST_CASE(
	"Ordered Dither exact colours do not consume missing Custom uniforms",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(1, 1, {64, 64, 64, 128});
	for (int64_t mapMode : {0, 1}) {
		auto run = RunNode(
			"pc.dither",
			{{"surface_in", &input}},
			{{"color_type", EnumValue{0}},
			 {"steps", int64_t{4}},
			 {"pattern", EnumValue{4}},
			 {"map_mode", EnumValue{mapMode}}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 64});
	}
}
TEST_CASE(
	"Ordered Dither linked mapped contrast pair remains one actual graph range",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	auto document = Graph();
	document.Nodes[1].Values.push_back({"contrast_mapped", true});
	Node range{"range", "pc.array", "", {}, {}, {}};
	range.DynamicInputs = {{"input_0", ValueType::Scalar, 0.0}, {"input_1", ValueType::Scalar, 20.0}};
	document.Nodes.push_back(std::move(range));
	document.Nodes.push_back(
		{"map",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{255, 255, 255, 255}}}}
	);
	document.Links.push_back({"map", "surface_out", "dither", "contrast_map"});
	document.Links.push_back({"range", "array", "dither", "contrast"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	auto inputs = EvaluateNodeInputs(document, plan, "dither", {}, snapshot, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(inputs == Status::Ok);
	bool linked = false;
	for (const auto &value : snapshot.Values())
		if (value.Port == "contrast") linked = value.Linked;
	CHECK(linked);
	Image image;
	auto status = Evaluate(document, plan, "out", {}, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CheckRows(image, {"##", "##"});
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	Plan compiled;
	REQUIRE(Compile(restored, compiled, diagnostic) == Status::Ok);
	Image again;
	REQUIRE(Evaluate(restored, compiled, "out", {}, again, diagnostic) == Status::Ok);
	CHECK(again == image);
}
TEST_CASE(
	"Ordered Dither masks apply after pattern selection", "[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2), mask = Solid(2, 2, {0, 0, 0, 255});
	auto hidden = RunNode(
		"pc.dither",
		{{"surface_in", &input}, {"mask", &mask}},
		{{"color_type", EnumValue{0}}, {"steps", int64_t{1}}, {"mask_alpha_only", false}}
	);
	REQUIRE(hidden.Ok);
	CHECK(hidden.Output().Pixels == input.Pixels);
	auto visible = RunNode(
		"pc.dither",
		{{"surface_in", &input}, {"mask", &mask}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"mask_alpha_only", false},
		 {"invert_mask", true}}
	);
	REQUIRE(visible.Ok);
	CheckRows(visible.Output(), {"#.", ".#"});
}
TEST_CASE(
	"Ordered Dither one-cell Linear matrix completes without positional division",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(2, 2);
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"pattern", EnumValue{5}},
		 {"map_mode", EnumValue{1}},
		 {"dither_matrix", MatrixValue{1, 1, {1}}}}
	);
	REQUIRE(run.Ok);
	CheckRows(run.Output(), {"..", ".."});
	auto positional = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"color_type", EnumValue{0}},
		 {"steps", int64_t{1}},
		 {"pattern", EnumValue{5}},
		 {"dither_matrix", MatrixValue{1, 1, {1}}}}
	);
	CHECK_FALSE(positional.Ok);
	CHECK(positional.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Ordered Dither exact palette match preserves input alpha before its square",
	"[imagegraph][source_2d][source_ordered_dither]"
) {
	const auto input = Solid(1, 1, {255, 255, 255, 128});
	auto run = RunNode(
		"pc.dither",
		{{"surface_in", &input}},
		{{"palette", ArrayValue{ValueType::Colour, {Colour{255, 255, 255, 64}}}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 64});
}
