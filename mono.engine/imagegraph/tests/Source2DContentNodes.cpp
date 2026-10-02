#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <string_view>

TEST_SUITE_ID("engine.imagegraph.source_content_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Solid(uint32_t width, uint32_t height, std::array<uint8_t, 4> rgba) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * 4);
		for (size_t at = 0; at < image.Pixels.size(); at += 4)
			for (size_t c = 0; c < 4; ++c)
				image.Pixels[at + c] = rgba[c];
		return image;
	}
	Image Pattern(std::initializer_list<std::string_view> rows) {
		Image image = Solid(uint32_t(rows.begin()->size()), uint32_t(rows.size()), {0, 0, 0, 255});
		size_t pixel = 0;
		for (auto row : rows)
			for (char value : row) {
				if (value == '#')
					for (size_t c = 0; c < 3; ++c)
						image.Pixels[pixel * 4 + c] = 255;
				++pixel;
			}
		return image;
	}
	Document Graph(std::string type) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"source",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{2, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{64, 128, 192, 128}}}},
			{"effect", std::move(type), "", {}, {}}
		};
		doc.Links = {{"source", "surface_out", "effect", "surface_in"}};
		doc.Outputs = {{"out", "effect", "surface_out"}};
		return doc;
	}
	Image EvaluateGraph(const Document &doc) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		Image image;
		auto status = Evaluate(doc, plan, "out", {}, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
} // namespace
TEST_CASE(
	"Gap Contract alternating source predicates retain thinning and "
	"expansion goldens",
	"[imagegraph][source_2d][source_content]"
) {
	const auto block = Pattern({".....", ".###.", ".###.", ".###.", "....."});
	struct Golden {
		int64_t Count;
		std::array<std::string_view, 5> Rows;
	};
	constexpr std::array expected{
		Golden{1, {".....", "..#..", ".##..", ".....", "....."}},
		Golden{2, {".....", ".....", "..#..", ".....", "....."}},
		Golden{3, {".....", ".....", "..#..", ".....", "....."}},
		Golden{-1, {".###.", "####.", "####.", "####.", "....."}},
		Golden{-2, {"#####", "#####", "#####", "#####", "####."}}
	};
	for (const auto &item : expected) {
		auto run = RunNode(
			"pc.gap_contract",
			{{"surface_in", &block}},
			{{"max_width", item.Count}, {"oversample", EnumValue{1}}}
		);
		INFO(item.Count);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto want = Pattern({item.Rows[0], item.Rows[1], item.Rows[2], item.Rows[3], item.Rows[4]});
		CHECK(run.Output().Pixels == want.Pixels);
	}
}
TEST_CASE(
	"Gap Contract zero width retains staging inversion before binary passes",
	"[imagegraph][source_2d][source_content]"
) {
	const auto image = Solid(2, 1, {64, 128, 192, 128});
	auto unchanged = RunNode("pc.gap_contract", {{"surface_in", &image}}, {{"max_width", int64_t{0}}});
	REQUIRE(unchanged.Ok);
	CHECK(unchanged.Output().Pixels == image.Pixels);
	auto invert =
		RunNode("pc.gap_contract", {{"surface_in", &image}}, {{"max_width", int64_t{0}}, {"invert", true}});
	REQUIRE(invert.Ok);
	CHECK(invert.Output().Pixels == Solid(2, 1, {191, 127, 63, 128}).Pixels);
}
TEST_CASE(
	"Gap Contract threshold multiplies light by alpha and Keep Alpha "
	"only changes alpha",
	"[imagegraph][source_2d][source_content]"
) {
	for (uint8_t alpha : {uint8_t{127}, uint8_t{128}}) {
		const auto image = Solid(1, 1, {255, 255, 255, alpha});
		auto run = RunNode(
			"pc.gap_contract",
			{{"surface_in", &image}},
			{{"max_width", int64_t{1}}, {"oversample", EnumValue{3}}, {"keep_alpha", true}}
		);
		REQUIRE(run.Ok);
		CHECK(
			run.Output().Pixels == Solid(
									   1,
									   1,
									   {uint8_t(alpha == 128 ? 255 : 0),
										uint8_t(alpha == 128 ? 255 : 0),
										uint8_t(alpha == 128 ? 255 : 0),
										alpha}
								   )
									   .Pixels
		);
	}
}
TEST_CASE(
	"Gap Contract oversampling selects real boundary neighbors", "[imagegraph][source_2d][source_content]"
) {
	const auto image = Solid(2, 2, {255, 255, 255, 255});
	auto empty = RunNode(
		"pc.gap_contract", {{"surface_in", &image}}, {{"max_width", int64_t{1}}, {"oversample", EnumValue{1}}}
	);
	REQUIRE(empty.Ok);
	CHECK(empty.Output().Pixels == Solid(2, 2, {0, 0, 0, 255}).Pixels);
	auto repeat = RunNode(
		"pc.gap_contract", {{"surface_in", &image}}, {{"max_width", int64_t{1}}, {"oversample", EnumValue{4}}}
	);
	REQUIRE(repeat.Ok);
	CHECK(repeat.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Gap Contract single-channel safe draw suppresses initial inversion",
	"[imagegraph][source_2d][source_content]"
) {
	Image image{1, 1, {64}, 0, SurfaceFormat::R8Unorm};
	auto run = RunNode(
		"pc.gap_contract",
		{{"surface_in", &image}},
		{{"max_width", int64_t{0}}, {"invert", true}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Gap Contract mask mix and inactive source branches retain original values",
	"[imagegraph][source_2d][source_content]"
) {
	const auto image = Solid(1, 1, {64, 128, 192, 128}), mask = Solid(1, 1, {0, 0, 0, 255});
	auto masked =
		RunNode("pc.gap_contract", {{"surface_in", &image}, {"mask", &mask}}, {{"max_width", int64_t{1}}});
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == image.Pixels);
	auto mixed =
		RunNode("pc.gap_contract", {{"surface_in", &image}}, {{"max_width", int64_t{1}}, {"mix", .5}});
	REQUIRE(mixed.Ok);
	CHECK(mixed.Output().Pixels == Solid(1, 1, {32, 64, 96, 192}).Pixels);
	auto inactive = RunNode(
		"pc.gap_contract", {{"surface_in", &image}}, {{"active", false}, {"max_width", int64_t{-64000001}}}
	);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Gap Contract actual graph persists controls and byte/work refusal "
	"is atomic",
	"[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.gap_contract");
	doc.Nodes[1].Values = {{"max_width", int64_t{0}}, {"invert", true}};
	const auto image = EvaluateGraph(doc);
	CHECK(image.Pixels == Solid(2, 1, {191, 127, 63, 128}).Pixels);
	Document read;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), read, diagnostic) == Status::Ok);
	CHECK(EvaluateGraph(read).Pixels == image.Pixels);
	Plan plan;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image retained = image;
	CHECK(Evaluate(doc, plan, "out", {}, retained, diagnostic, 1) == Status::LimitExceeded);
	CHECK(retained == image);
	doc.Nodes[1].Values[0].Data = int64_t{64000000};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(doc, plan, "out", {}, retained, diagnostic) == Status::LimitExceeded);
	CHECK(retained == image);
}
TEST_CASE(
	"Gap Contract actual processor leaves have independent binary iterations",
	"[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.gap_contract");
	Node values{"values", "pc.array", "", {}, {}, {}};
	values.DynamicInputs = {
		{"input_0", ValueType::Integer, int64_t{0}}, {"input_1", ValueType::Integer, int64_t{1}}
	};
	doc.Nodes.push_back(values);
	doc.Links.push_back({"values", "array", "effect", "max_width"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(doc, plan, "out", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels == Solid(2, 1, {64, 128, 192, 128}).Pixels);
	CHECK(images.Images[1].Pixels == Solid(2, 1, {0, 0, 0, 255}).Pixels);
}
TEST_CASE(
	"Align Content moves byte-alpha bounds to each canvas anchor", "[imagegraph][source_2d][source_content]"
) {
	auto image = Solid(5, 3, {0, 0, 0, 0});
	image.Pixels[0] = 255;
	image.Pixels[3] = 255;
	for (Vector2 anchor : {Vector2{0, 0}, Vector2{.5, .5}, Vector2{1, 1}}) {
		auto run = RunNode("pc.align_content", {{"surface_in", &image}}, {{"align_anchor", anchor}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		auto expected = Solid(5, 3, {0, 0, 0, 0});
		const size_t at = (size_t(anchor.Y * 2) * 5 + size_t(anchor.X * 4)) * 4;
		expected.Pixels[at] = expected.Pixels[at + 3] = 255;
		CHECK(run.Output().Pixels == expected.Pixels);
	}
}
TEST_CASE(
	"Align Content IPadding rounds after reference width and height conversion",
	"[imagegraph][source_2d][source_content]"
) {
	auto image = Solid(5, 3, {0, 0, 0, 0});
	image.Pixels[0] = 255;
	image.Pixels[3] = 255;
	auto run = RunNode(
		"pc.align_content",
		{{"surface_in", &image}},
		{{"align_anchor", Vector2{0, 0}},
		 {"pad_content", Vector4{0, .5, .5, 0}},
		 {"pad_content_unit", EnumValue{1}}}
	);
	REQUIRE(run.Ok);
	auto expected = Solid(5, 3, {0, 0, 0, 0});
	expected.Pixels[(2 * 5 + 2) * 4] = expected.Pixels[(2 * 5 + 2) * 4 + 3] = 255;
	CHECK(run.Output().Pixels == expected.Pixels);
}
TEST_CASE(
	"Align Content removes only exact background equality before bounds scan",
	"[imagegraph][source_2d][source_content]"
) {
	auto image = Solid(3, 1, {64, 0, 0, 255});
	image.Pixels[4] = 65;
	auto run = RunNode(
		"pc.align_content",
		{{"surface_in", &image}},
		{{"background", Colour{64, 0, 0, 255}}, {"align_anchor", Vector2{1, 0}}}
	);
	REQUIRE(run.Ok);
	auto expected = Solid(3, 1, {64, 0, 0, 255});
	expected.Pixels[8] = 65;
	CHECK(run.Output().Pixels == expected.Pixels);
}
TEST_CASE(
	"Align Content preserves source alpha-add drawing over its cleared "
	"background",
	"[imagegraph][source_2d][source_content]"
) {
	const auto image = Solid(1, 1, {64, 0, 0, 128});
	auto run = RunNode("pc.align_content", {{"surface_in", &image}}, {{"background", Colour{64, 0, 0, 128}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == Solid(1, 1, {64, 0, 0, 128}).Pixels);
	auto different =
		RunNode("pc.align_content", {{"surface_in", &image}}, {{"background", Colour{32, 0, 0, 64}}});
	REQUIRE(different.Ok);
	CHECK(different.Output().Pixels == Solid(1, 1, {80, 0, 0, 192}).Pixels);
}
TEST_CASE(
	"Align Content single-channel draw bypasses replace-color shader",
	"[imagegraph][source_2d][source_content]"
) {
	Image image{1, 1, {64}, 0, SurfaceFormat::R8Unorm};
	auto run = RunNode(
		"pc.align_content",
		{{"surface_in", &image}},
		{{"background", Colour{64, 64, 64, 255}}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Align Content HDR alpha is quantized before native bbox scanning",
	"[imagegraph][source_2d][source_content]"
) {
	Image image{2, 1, std::vector<uint8_t>(32), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(image, 0, 0, {0, 0, 0, .001}));
	REQUIRE(StoreSurfacePixel(image, 1, 0, {1, 0, 0, 1}));
	auto run = RunNode("pc.align_content", {{"surface_in", &image}}, {{"align_anchor", Vector2{0, 0}}});
	REQUIRE(run.Ok);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, pixel));
	CHECK(pixel == SurfacePixel{1, 0, 0, 1});
}
TEST_CASE(
	"Align Content actual graph persistence and geometric refusal "
	"preserve output",
	"[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.align_content");
	const auto image = EvaluateGraph(doc);
	CHECK(image.Pixels == Solid(2, 1, {64, 128, 192, 128}).Pixels);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	CHECK(EvaluateGraph(restored).Pixels == image.Pixels);
	Plan plan;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image retained = image;
	CHECK(Evaluate(doc, plan, "out", {}, retained, diagnostic, 1) == Status::LimitExceeded);
	CHECK(retained == image);
	doc.Nodes[1].Values.push_back({"pad_content", Vector4{0, 0, 1000000, 0}});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(doc, plan, "out", {}, retained, diagnostic) == Status::LimitExceeded);
	CHECK(retained == image);
}
TEST_CASE(
	"Gap Contract submitted interpolation is inert for its plain shader "
	"texture reads",
	"[imagegraph][source_2d][source_content]"
) {
	const auto image = Pattern({".....", ".###.", ".###.", ".###.", "....."});
	auto pixel = RunNode(
		"pc.gap_contract",
		{{"surface_in", &image}},
		{{"max_width", int64_t{1}}, {"oversample", EnumValue{1}}, {"interpolate", EnumValue{1}}}
	);
	auto cubic = RunNode(
		"pc.gap_contract",
		{{"surface_in", &image}},
		{{"max_width", int64_t{1}}, {"oversample", EnumValue{1}}, {"interpolate", EnumValue{3}}}
	);
	REQUIRE(pixel.Ok);
	REQUIRE(cubic.Ok);
	CHECK(pixel.Output().Pixels == cubic.Output().Pixels);
}
TEST_CASE(
	"Content kernel whole processor array work refusal preserves the "
	"prior array",
	"[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.gap_contract");
	Node values{"values", "pc.array", "", {}, {}, {}};
	for (size_t i = 0; i < 64; ++i)
		values.DynamicInputs.push_back({"input_" + std::to_string(i), ValueType::Integer, int64_t{1}});
	doc.Nodes.push_back(values);
	doc.Links.push_back({"values", "array", "effect", "max_width"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray images;
	REQUIRE(EvaluateArray(doc, plan, "out", {}, images, diagnostic) == Status::Ok);
	const auto previous = images;
	doc.Nodes[0].Values[0].Data = Vector2{512, 512};
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateArray(doc, plan, "out", {}, images, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "effect");
	CHECK(diagnostic.Port == "max_width");
	CHECK(images.Images == previous.Images);
	CHECK(images.Items == previous.Items);
}
TEST_CASE(
	"Align Content inactive copy ignores otherwise refused geometric controls",
	"[imagegraph][source_2d][source_content]"
) {
	const auto image = Solid(2, 1, {64, 128, 192, 128});
	auto run = RunNode(
		"pc.align_content",
		{{"surface_in", &image}},
		{{"active", false}, {"pad_content", Vector4{0, 0, 1000000, 0}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Align Content negative padding can clip content rather than resize "
	"the canvas",
	"[imagegraph][source_2d][source_content]"
) {
	auto image = Solid(3, 1, {0, 0, 0, 0});
	image.Pixels[0] = 255;
	image.Pixels[3] = 255;
	auto run = RunNode(
		"pc.align_content",
		{{"surface_in", &image}},
		{{"align_anchor", Vector2{0, 0}}, {"pad_content", Vector4{0, 0, -1, 0}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 3);
	CHECK(run.Output().Pixels == std::vector<uint8_t>(12));
}
TEST_CASE(
	"Align Content processor anchor rows preserve independent placements",
	"[imagegraph][source_2d][source_content]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}}},
		{"canvas", "pc.crop", "", {}, {{"crop", Vector4{-2, 0, 0, 0}}, {"crop_unit", EnumValue{0}}}},
		{"align", "pc.align_content", "", {}, {}}
	};
	doc.Links = {
		{"source", "surface_out", "canvas", "surface_in"}, {"canvas", "surface_out", "align", "surface_in"}
	};
	doc.Outputs = {{"out", "align", "surface_out"}};
	Node anchors{"anchors", "pc.array", "", {}, {}, {}};
	anchors.DynamicInputs = {
		{"input_0", ValueType::Vector2, Vector2{0, 0}}, {"input_1", ValueType::Vector2, Vector2{1, 0}}
	};
	doc.Nodes.push_back(anchors);
	doc.Links.push_back({"anchors", "array", "align", "align_anchor"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(doc, plan, "out", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels[0] == 255);
	CHECK(images.Images[1].Pixels[8] == 255);
}

TEST_CASE(
	"Align Content linked scalar IPadding repeats before rounding", "[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.align_content");
	doc.Nodes[0].Values = {
		{"dimension", Vector2{5, 3}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}
	};
	doc.Nodes.insert(
		doc.Nodes.begin() + 1,
		Node{"canvas", "pc.crop", "", {}, {{"crop", Vector4{-4, 0, 0, -2}}, {"crop_unit", EnumValue{0}}}}
	);
	doc.Nodes[0].Values[0].Data = Vector2{1, 1};
	doc.Links = {
		{"source", "surface_out", "canvas", "surface_in"},
		{"canvas", "surface_out", "effect", "surface_in"},
		{"padding", "number", "effect", "pad_content"}
	};
	doc.Nodes.push_back(Node{"padding", "pc.number_simple", "", {}, {{"value", 1.5}}});
	doc.Nodes[2].Values = {{"align_anchor", Vector2{0, 0}}};
	const auto image = EvaluateGraph(doc);
	CHECK(image.Pixels[(2 * 5 + 2) * 4] == 255);
	CHECK(image.Pixels[0] == 0);
}
TEST_CASE(
	"Align Content short linked IPadding tuples are zero-padded", "[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.align_content");
	doc.Nodes[0].Values = {
		{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}
	};
	doc.Nodes.insert(
		doc.Nodes.begin() + 1,
		Node{"canvas", "pc.crop", "", {}, {{"crop", Vector4{-4, 0, 0, -2}}, {"crop_unit", EnumValue{0}}}}
	);
	Node padding{"padding", "pc.array", "", {}, {}};
	padding.DynamicInputs = {{"input_0", ValueType::Scalar, 2.0}, {"input_1", ValueType::Scalar, 1.0}};
	doc.Nodes.push_back(padding);
	doc.Links = {
		{"source", "surface_out", "canvas", "surface_in"},
		{"canvas", "surface_out", "effect", "surface_in"},
		{"padding", "array", "effect", "pad_content"}
	};
	doc.Nodes[2].Values = {{"align_anchor", Vector2{0, 0}}};
	const auto image = EvaluateGraph(doc);
	CHECK(image.Pixels[(1 * 5) * 4] == 255);
	CHECK(image.Pixels[0] == 0);
}
TEST_CASE(
	"Align Content nested IPadding rows preserve source row selection",
	"[imagegraph][source_2d][source_content]"
) {
	auto doc = Graph("pc.align_content");
	doc.Nodes[0].Values = {
		{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}
	};
	doc.Nodes.insert(
		doc.Nodes.begin() + 1,
		Node{"canvas", "pc.crop", "", {}, {{"crop", Vector4{-4, 0, 0, -2}}, {"crop_unit", EnumValue{0}}}}
	);
	Node padding{"padding", "pc.array", "", {}, {}};
	padding.DynamicInputs = {
		{"input_0", ValueType::Vector4, Vector4{0, 0, 0, 0}},
		{"input_1", ValueType::Vector4, Vector4{0, 1, 2, 0}}
	};
	doc.Nodes.push_back(padding);
	doc.Links = {
		{"source", "surface_out", "canvas", "surface_in"},
		{"canvas", "surface_out", "effect", "surface_in"},
		{"padding", "array", "effect", "pad_content"}
	};
	doc.Nodes[2].Values = {{"align_anchor", Vector2{0, 0}}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray images;
	auto status = EvaluateArray(doc, plan, "out", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels[0] == 255);
	CHECK(images.Images[1].Pixels[(1 * 5 + 2) * 4] == 255);
}

TEST_CASE(
	"Gap Contract mask inversion and feathering use the source modifier pipeline",
	"[imagegraph][source_2d][source_content]"
) {
	const auto image = Solid(1, 1, {64, 128, 192, 128}), mask = Solid(1, 1, {0, 0, 0, 255});
	auto inverted = RunNode(
		"pc.gap_contract",
		{{"surface_in", &image}, {"mask", &mask}},
		{{"max_width", int64_t{1}}, {"invert_mask", true}}
	);
	REQUIRE(inverted.Ok);
	CHECK(inverted.Output().Pixels == Solid(1, 1, {0, 0, 0, 255}).Pixels);
	auto feathered = RunNode(
		"pc.gap_contract",
		{{"surface_in", &image}, {"mask", &mask}},
		{{"max_width", int64_t{1}}, {"invert_mask", true}, {"mask_feather", 1.0}}
	);
	REQUIRE(feathered.Ok);
	CHECK(feathered.Output().Pixels == inverted.Output().Pixels);
	auto refused = RunNode(
		"pc.gap_contract",
		{{"surface_in", &image}, {"mask", &mask}},
		{{"max_width", int64_t{0}}, {"mask_feather", 65.0}}
	);
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::LimitExceeded);
}
