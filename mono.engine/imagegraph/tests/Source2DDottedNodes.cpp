#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_dotted")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document DottedGraph(Vector2 dimension = {4, 4}) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"dots",
			 "pc.dotted",
			 "",
			 {},
			 {{"dimension", dimension},
			  {"dimension_unit", EnumValue{0}},
			  {"size", 4.0},
			  {"size_unit", EnumValue{0}},
			  {"dot_size", .75},
			  {"seed", 17.0},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		d.Outputs = {{"out", "dots", "surface_out"}};
		return d;
	}
	void DottedSet(Document &d, std::string port, Value value) {
		auto &v = d.Nodes[0].Values;
		for (auto &item : v)
			if (item.Port == port) {
				item.Data = std::move(value);
				return;
			}
		v.push_back({std::move(port), std::move(value)});
	}
	Image DottedEvaluate(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto compiled = Compile(d, p, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto status = Evaluate(d, p, "out", {}, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void DottedRows(const Image &image, std::initializer_list<std::initializer_list<int>> rows) {
		REQUIRE(image.Height == rows.size());
		size_t y = 0;
		for (auto row : rows) {
			REQUIRE(row.size() == image.Width);
			size_t x = 0;
			for (int value : row) {
				for (size_t channel = 0; channel < 3; ++channel)
					CHECK(image.Pixels[(y * image.Width + x) * 4 + channel] == value);
				CHECK(image.Pixels[(y * image.Width + x) * 4 + 3] == 255);
				++x;
			}
			++y;
		}
	}
}
TEST_CASE("Dotted Grid and Hexagonal preserve literal native shader coverage", "[source_dotted]") {
	auto d = DottedGraph({8, 6});
	DottedRows(
		DottedEvaluate(d),
		{{0, 0, 0, 0, 0, 0, 0, 0},
		 {0, 255, 255, 0, 0, 255, 255, 0},
		 {0, 255, 255, 0, 0, 255, 255, 0},
		 {0, 0, 0, 0, 0, 0, 0, 0},
		 {0, 0, 0, 0, 0, 0, 0, 0},
		 {0, 255, 255, 0, 0, 255, 255, 0}}
	);
	DottedSet(d, "pattern", EnumValue{1});
	DottedRows(
		DottedEvaluate(d),
		{{0, 255, 255, 0, 0, 255, 255, 0},
		 {0, 255, 255, 0, 0, 255, 255, 0},
		 {0, 255, 255, 0, 0, 255, 255, 0},
		 {0, 0, 0, 0, 0, 0, 0, 0},
		 {255, 0, 0, 255, 255, 0, 0, 255},
		 {255, 0, 0, 255, 255, 0, 0, 255}}
	);
}
TEST_CASE("Dotted AA and Smooth retain smoothstep and unclamped distance formulas", "[source_dotted]") {
	auto d = DottedGraph();
	DottedSet(d, "render_mode", EnumValue{1});
	DottedSet(d, "dot_size", .5);
	DottedSet(d, "smoothness", .5);
	DottedRows(DottedEvaluate(d), {{0, 97, 97, 0}, {97, 255, 255, 97}, {97, 255, 255, 97}, {0, 97, 97, 0}});
	DottedSet(d, "render_mode", EnumValue{2});
	DottedSet(d, "dot_size", .75);
	DottedRows(DottedEvaluate(d), {{0, 0, 0, 0}, {0, 101, 101, 0}, {0, 101, 101, 0}, {0, 0, 0, 0}});
}
TEST_CASE("Dotted additive Solid colours exist outside dot coverage", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "dot_size", 0.0);
	DottedSet(d, "dot_color", Colour{255, 0, 0, 255});
	DottedSet(d, "bg_color", Colour{0, 0, 255, 255});
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	DottedSet(d, "blend_mode", EnumValue{1});
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{255, 0, 255, 255});
}
TEST_CASE("Dotted Normal blend divides by composed alpha without source alpha squaring", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "size", 1.0);
	DottedSet(d, "dot_color", Colour{255, 0, 0, 128});
	DottedSet(d, "bg_color", Colour{0, 0, 255, 128});
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{170, 0, 85, 192});
	DottedSet(d, "bg_color", Colour{0, 0, 0, 0});
	DottedSet(d, "dot_color", Colour{0, 0, 0, 0});
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{0, 0, 0, 0});
}
TEST_CASE("Dotted palette indexes repeat row plus column and remain source ordered", "[source_dotted]") {
	auto d = DottedGraph({4, 1});
	DottedSet(d, "size", 1.0);
	DottedSet(d, "dot_color_mode", EnumValue{1});
	DottedSet(
		d, "palette", ArrayValue{ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 0, 255, 255}}, {}}
	);
	CHECK(
		DottedEvaluate(d).Pixels ==
		std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255}
	);
}
TEST_CASE("Dotted Random gradient uses the shader hash and continuous key interpolation", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "size", 1.0);
	DottedSet(d, "dot_color_mode", EnumValue{2});
	DottedSet(d, "shift", .25);
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	DottedSet(d, "shift", 1.25);
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	// Cell (0,0) hashes to zero for every seed. A constant gradient covers nonzero hash cells too.
	DottedSet(d, "dimension", Vector2{3, 2});
	DottedSet(d, "gradient", Gradient{0, {{0, {11, 22, 33, 255}}, {1, {11, 22, 33, 255}}}});
	const auto image = DottedEvaluate(d);
	for (size_t i = 0; i < image.Pixels.size(); i += 4) {
		CHECK(image.Pixels[i] == 11);
		CHECK(image.Pixels[i + 1] == 22);
		CHECK(image.Pixels[i + 2] == 33);
	}
}
TEST_CASE("Dotted Texture samples cell origins using nearest clamp", "[source_dotted]") {
	Image texture{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}, 0};
	auto run = RunNode(
		"pc.dotted",
		{{"texture", &texture}},
		{{"dimension", Vector2{4, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"dot_color_mode", EnumValue{3}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(
		run.Output().Pixels ==
		std::vector<uint8_t>{255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255}
	);
}
TEST_CASE(
	"Dotted Texture without a linked sampler refuses the ambient branch atomically", "[source_dotted]"
) {
	auto d = DottedGraph();
	DottedSet(d, "dot_color_mode", EnumValue{3});
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image output{1, 1, {9, 8, 7, 6}, 0};
	const Image prior = output;
	CHECK(Evaluate(d, p, "out", {}, output, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "texture");
	CHECK(output == prior);
}
TEST_CASE("Dotted Reference scalar Size differs from mapped Float endpoints", "[source_dotted]") {
	auto d = DottedGraph();
	DottedSet(d, "size", .25);
	DottedSet(d, "size_unit", EnumValue{1});
	DottedRows(
		DottedEvaluate(d),
		{{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}}
	);
	DottedSet(d, "size_mapped", true);
	Node range{"range", "pc.array", "", {}, {}, {}};
	range.DynamicInputs = {{"input_0", ValueType::Scalar, 4.0}, {"input_1", ValueType::Scalar, 4.0}};
	d.Nodes.push_back(std::move(range));
	d.Links = {{"range", "array", "dots", "size"}};
	DottedRows(DottedEvaluate(d), {{0, 0, 0, 0}, {0, 255, 255, 0}, {0, 255, 255, 0}, {0, 0, 0, 0}});
}
TEST_CASE("Dotted mapped Size angle and dot radius preserve linked two endpoint arrays", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "position", Vector2{.25, 0});
	DottedSet(d, "position_unit", EnumValue{0});
	DottedSet(d, "size", 1.0);
	Node range{"range", "pc.array", "", {}, {}, {}};
	range.DynamicInputs = {{"input_0", ValueType::Scalar, 0.0}, {"input_1", ValueType::Scalar, 1.0}};
	d.Nodes.push_back(std::move(range));
	d.Nodes.push_back(
		{"map",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{255, 255, 255, 0}}}}
	);
	DottedSet(d, "dot_size_mapped", true);
	d.Links = {{"range", "array", "dots", "dot_size"}, {"map", "surface_out", "dots", "dot_size_map"}};
	const auto whiteMap = DottedEvaluate(d);
	CHECK(whiteMap.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	// Map alpha is ignored by the numeric control's mean-RGB read.
	d.Nodes[2].Values[2].Data = Colour{0, 0, 0, 255};
	const auto blackMap = DottedEvaluate(d);
	CHECK(blackMap.Pixels == std::vector<uint8_t>{0, 0, 0, 255});
}
TEST_CASE("Dotted mapped flags without map use the first source endpoint", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "size", 1.0);
	DottedSet(d, "dot_size_mapped", true);
	DottedSet(d, "position", Vector2{.25, 0});
	DottedSet(d, "position_unit", EnumValue{0});
	Node range{"range", "pc.array", "", {}, {}, {}};
	range.DynamicInputs = {{"input_0", ValueType::Scalar, .75}, {"input_1", ValueType::Scalar, 0.0}};
	d.Nodes.push_back(std::move(range));
	d.Links = {{"range", "array", "dots", "dot_size"}};
	const auto first = DottedEvaluate(d);
	CHECK(first.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	d.Nodes[1].DynamicInputs[0].Default = 0.0;
	d.Nodes[1].DynamicInputs[1].Default = .75;
	const auto second = DottedEvaluate(d);
	CHECK(second.Pixels == std::vector<uint8_t>{0, 0, 0, 255});
}
TEST_CASE("Dotted UV alpha and Mask Empty retain separate alpha multiplication", "[source_dotted]") {
	const Image uv{1, 1, {128, 128, 0, 128}, 0}, mask{1, 1, {128, 128, 128, 128}, 0};
	auto run = RunNode(
		"pc.dotted",
		{{"uv_map", &uv}, {"mask", &mask}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"uv_mix", 0.0},
		 {"mask_alpha_only", true},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 32});
}
TEST_CASE("Dotted floating and red surfaces preserve Mask Empty RGBA8 intermediate", "[source_dotted]") {
	const Image blackMask{1, 1, {0, 0, 0, 255}, 0};
	for (int64_t depth : {5, 7, 8}) {
		auto run = RunNode(
			"pc.dotted",
			{{"mask", &blackMask}},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"size", 1.0},
			 {"size_unit", EnumValue{0}},
			 {"bg_color", Colour{0, 0, 0, 0}},
			 {"dot_color", Colour{64, 0, 0, 255}},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		SurfacePixel p{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, p));
		CHECK(p[0] == Catch::Approx(64 / 255.0).margin(.0002));
		if (depth == 5)
			CHECK(p[3] == 0);
		else
			CHECK(p[3] == 1);
	}
}
TEST_CASE(
	"Dotted zero divisors and undefined AA edges refuse selected source arithmetic", "[source_dotted]"
) {
	for (auto field : {"size", "spacing", "smoothness"}) {
		auto d = DottedGraph();
		if (std::string_view(field) == "spacing")
			DottedSet(d, field, Vector2{0, 1});
		else
			DottedSet(d, field, 0.0);
		if (std::string_view(field) == "smoothness") DottedSet(d, "render_mode", EnumValue{1});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		Image out;
		CHECK(Evaluate(d, p, "out", {}, out, diag) == Status::UnsupportedExecution);
	}
}
TEST_CASE(
	"Dotted choices numeric fields and source unit metadata survive native persistence", "[source_dotted]"
) {
	auto d = DottedGraph({8, 6});
	DottedSet(d, "pattern", EnumValue{1});
	DottedSet(d, "position", Vector2{.25, .1});
	DottedSet(d, "position_unit", EnumValue{1});
	DottedSet(d, "angle", 30.0);
	DottedSet(d, "spacing", Vector2{1, 2});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(DottedEvaluate(restored) == DottedEvaluate(d));
}
TEST_CASE(
	"Dotted actual heterogeneous processor dimension rows match scalar graph oracles", "[source_dotted]"
) {
	auto d = DottedGraph();
	Node dimensions{"dimensions", "pc.array", "", {}, {}, {}};
	dimensions.DynamicInputs = {
		{"input_0", ValueType::Vector2, Vector2{4, 4}}, {"input_1", ValueType::Vector2, Vector2{8, 6}}
	};
	d.Nodes.push_back(std::move(dimensions));
	d.Links = {{"dimensions", "array", "dots", "dimension"}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	ImageArray out;
	auto status = EvaluateArray(d, p, "out", {}, out, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(out.Images.size() == 2);
	CHECK(out.Images[0] == DottedEvaluate(DottedGraph({4, 4})));
	CHECK(out.Images[1] == DottedEvaluate(DottedGraph({8, 6})));
}
TEST_CASE(
	"Dotted later expensive Size row work is refused before first output allocation", "[source_dotted]"
) {
	auto d = DottedGraph({640, 640});
	Node sizes{"sizes", "pc.array", "", {}, {}, {}};
	sizes.DynamicInputs = {{"input_0", ValueType::Scalar, 640.0}, {"input_1", ValueType::Scalar, .01}};
	d.Nodes.push_back(std::move(sizes));
	d.Links = {{"sizes", "array", "dots", "size"}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image out{1, 1, {9, 8, 7, 6}, 0};
	const auto previous = out;
	CHECK(Evaluate(d, p, "out", {}, out, diag, 500000) == Status::LimitExceeded);
	CHECK(diag.NodeId == "dots");
	CHECK(diag.Message.find("whole-array work") != std::string::npos);
	CHECK(out == previous);
}
TEST_CASE("Dotted output budget refusal preserves previous public image", "[source_dotted]") {
	auto d = DottedGraph();
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image out{1, 1, {9, 8, 7, 6}, 0};
	const auto previous = out;
	CHECK(Evaluate(d, p, "out", {}, out, diag, 1) == Status::LimitExceeded);
	CHECK(out == previous);
}
TEST_CASE("Dotted original UV map green inversion and mapped Angle remain independent", "[source_dotted]") {
	const Image white{1, 1, {255, 255, 255, 255}, 0}, black{1, 1, {0, 0, 0, 255}, 0};
	for (const auto *map : {&black, &white}) {
		auto run = RunNode(
			"pc.dotted",
			{{"angle_map", map}},
			{{"dimension", Vector2{4, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"size", 1.0},
			 {"size_unit", EnumValue{0}},
			 {"dot_size", .75},
			 {"angle_mapped", true},
			 {"angle", Vector2{0, 90}},
			 {"attribute_color_depth", EnumValue{3}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		for (size_t i = 0; i < run.Output().Pixels.size(); i += 4)
			CHECK(run.Output().Pixels[i] == (map == &black ? 255 : 0));
	}
	const Image uv{1, 1, {128, 128, 0, 128}, 0};
	auto run = RunNode(
		"pc.dotted",
		{{"uv_map", &uv}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 128});
}
TEST_CASE(
	"Dotted linked mapped Size pair preserves Reference units and selected map value", "[source_dotted]"
) {
	auto d = DottedGraph({4, 1});
	DottedSet(d, "size_unit", EnumValue{1});
	DottedSet(d, "size_mapped", true);
	Node range{"sizes", "pc.array", "", {}, {}, {}};
	range.DynamicInputs = {{"input_0", ValueType::Scalar, 1.0}, {"input_1", ValueType::Scalar, 4.0}};
	d.Nodes.push_back(std::move(range));
	d.Nodes.push_back(
		{"map",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{0, 0, 0, 255}}}}
	);
	d.Links = {{"sizes", "array", "dots", "size"}, {"map", "surface_out", "dots", "size_map"}};
	CHECK(
		DottedEvaluate(d).Pixels ==
		std::vector<uint8_t>{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}
	);
	d.Nodes[2].Values[2].Data = Colour{255, 255, 255, 255};
	CHECK(
		DottedEvaluate(d).Pixels ==
		std::vector<uint8_t>{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255}
	);
}
TEST_CASE("Dotted Mask units multiply dimensions with source half-even rounding", "[source_dotted]") {
	const Image mask{3, 2, std::vector<uint8_t>(3 * 2 * 4, 255), 0};
	auto run = RunNode(
		"pc.dotted",
		{{"mask", &mask}},
		{{"dimension", Vector2{1.5, 1.25}},
		 {"dimension_unit", EnumValue{2}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 4);
	CHECK(run.Output().Height == 2);
}
TEST_CASE(
	"Dotted empty runtime palette and authored gradient keep distinct admission boundaries", "[source_dotted]"
) {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "dot_color_mode", EnumValue{1});
	DottedSet(d, "palette", ArrayValue{ValueType::Colour, {}, {}});
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "palette");
	CHECK(image == prior);
	const auto paletteDocument = d;
	DottedSet(d, "dot_color_mode", EnumValue{2});
	DottedSet(d, "gradient", Gradient{});
	CHECK(Compile(d, p, diag) == Status::InvalidValue);
	CHECK(diag.NodeId == "dots");
	CHECK(diag.Port == "gradient");
	CHECK(Evaluate(paletteDocument, p, "out", {}, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "palette");
	CHECK(image == prior);
	// Authored empty keys are invalid even when the selected coloring branch does not read them.
	DottedSet(d, "dot_color_mode", EnumValue{0});
	CHECK(Compile(d, p, diag) == Status::InvalidValue);
	std::erase_if(d.Nodes[0].Values, [](const AuthoredValue &v) { return v.Port == "gradient"; });
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::Ok);
}
TEST_CASE("Dotted zero Pixel threshold still covers its exact center", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "size", 1.0);
	DottedSet(d, "dot_size", 0.0);
	const auto image = DottedEvaluate(d);
	CHECK(image.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}
TEST_CASE("Dotted negative size preserves the source empty neighbour loop", "[source_dotted]") {
	auto d = DottedGraph({1, 1});
	DottedSet(d, "size", -1.0);
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	DottedSet(d, "blend_mode", EnumValue{1});
	DottedSet(d, "dot_color", Colour{9, 8, 7, 255});
	CHECK(DottedEvaluate(d).Pixels == std::vector<uint8_t>{9, 8, 7, 255});
}
TEST_CASE("Dotted selected surface formats retain native storage and quantization", "[source_dotted]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto d = DottedGraph({1, 1});
		DottedSet(d, "size", 1.0);
		DottedSet(d, "dot_color", Colour{64, 128, 192, 255});
		DottedSet(d, "attribute_color_depth", EnumValue{depth});
		const auto image = DottedEvaluate(d);
		REQUIRE(SourceSurfaceFormat(depth));
		CHECK(image.Format == *SourceSurfaceFormat(depth));
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
		const double red = depth == 2 ? 4 / 15.0 : 64 / 255.0;
		CHECK(pixel[0] == Catch::Approx(red).margin(.0002));
		if (depth >= 6) {
			CHECK(pixel[1] == 0);
			CHECK(pixel[2] == 0);
		} else {
			CHECK(pixel[1] == Catch::Approx(depth == 2 ? 8 / 15.0 : 128 / 255.0).margin(.0003));
			CHECK(pixel[2] == Catch::Approx(depth == 2 ? 11 / 15.0 : 192 / 255.0).margin(.0003));
		}
	}
}

TEST_CASE(
	"Dotted source Color Depth remains a static attribute rather than an array input", "[source_dotted]"
) {
	const auto *entry = FindCatalogueEntry("pc.dotted");
	REQUIRE(entry);
	const auto *depth = FindCatalogueInput(*entry, "attribute_color_depth");
	REQUIRE(depth);
	CHECK(depth->SourceIndex == -1);
	CHECK(depth->ArrayDepth == 0);
	CHECK_FALSE(depth->SourceBehavior);
	auto d = DottedGraph({1, 1});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	Image output = DottedEvaluate(d);
	const Image prior = output;
	const auto valid = d;
	SECTION("Authored later extreme attribute array is rejected at compile") {
		DottedSet(
			d,
			"attribute_color_depth",
			ArrayValue{ValueType::Scalar, {3.0, std::numeric_limits<double>::max()}, {}}
		);
		CHECK(Compile(d, plan, diagnostic) == Status::TypeMismatch);
		CHECK(diagnostic.NodeId == "dots");
		CHECK(diagnostic.Port == "attribute_color_depth");
	}
	SECTION("Linked depth arrays require unsupported conditional format routing") {
		Node depths{"depths", "pc.array", "", {}, {}, {}};
		depths.DynamicInputs = {
			{"input_0", ValueType::Scalar, 3.0},
			{"input_1", ValueType::Scalar, std::numeric_limits<double>::max()}
		};
		d.Nodes.push_back(std::move(depths));
		d.Links = {{"depths", "array", "dots", "attribute_color_depth"}};
		const auto status = Compile(d, plan, diagnostic);
		CHECK(status == Status::UnsupportedExecution);
		CHECK(diagnostic.NodeId == "dots");
		CHECK(diagnostic.Port == "attribute_color_depth");
	}
	CHECK(output == prior);
	// A refused compile cannot replace the previously accepted plan or public image.
	REQUIRE(Evaluate(valid, plan, "out", {}, output, diagnostic) == Status::Ok);
	CHECK(output == prior);
}
