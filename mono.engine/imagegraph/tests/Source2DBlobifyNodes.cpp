#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <cfenv>
#include <cmath>
TEST_SUITE_ID("engine.imagegraph.source_blobify")
using namespace complex_generator_test;
namespace {
	Document Blobify(int shape = 0) {
		Document d;
		d.FormatVersion = 9;
		MatrixValue matrix{4, 4, {}};
		ArrayValue palette{ValueType::Colour, {}};
		for (int i = 0; i < 16; ++i) {
			matrix.Values.push_back(i);
			palette.Elements.emplace_back(
				Colour{uint8_t(i * 13), uint8_t((15 - i) * 13), uint8_t((i % 4) * 60), 255}
			);
		}
		d.Nodes = {
			{"generator",
			 "pc.blobify",
			 "",
			 {},
			 {{"shape", EnumValue{shape}},
			  {"radius", int64_t{2}},
			  {"interpolate", EnumValue{1}},
			  {"oversample", EnumValue{3}}}},
			{"source",
			 "pc.interpret_matrix",
			 "",
			 {},
			 {{"matrix", matrix},
			  {"palette", palette},
			  {"mode", EnumValue{1}},
			  {"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		Set(d, "threshold", .25);
		Set(d, "smoothness", .5);
		d.Links = {{"source", "surface_out", "generator", "surface_in"}};
		d.Outputs = {{"out", "generator", "surface_out"}};
		return d;
	}
	Image Source(Document d) {
		d.Outputs = {{"out", "source", "surface_out"}};
		return Draw(d);
	}
	void Same(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		const bool pixels = actual.Pixels == expected.Pixels;
		CHECK(pixels);
		CHECK(actual.Hash == SurfaceHash(actual));
		CHECK(actual == expected);
	}
	void Erase(Document &d, std::string_view port) {
		auto &v = d.Nodes[0].Values;
		std::erase_if(v, [&](const auto &n) { return n.Port == port; });
	}
	Image Golden(int shape, bool distance = false) {
		static const std::array<std::vector<uint8_t>, 6> pixels{
			std::vector<uint8_t>{0,	 108, 0, 141, 10,  135, 44, 189, 24,  155, 110, 234, 39,  155, 179, 253,
								 29, 79,  0, 141, 48,  96,	44, 189, 72,  107, 110, 234, 90,  103, 179, 253,
								 58, 50,  0, 141, 87,  58,	44, 189, 119, 60,  110, 234, 142, 52,  179, 253,
								 86, 22,  0, 141, 125, 19,	44, 189, 167, 12,  110, 234, 194, 0,   179, 253},
			std::vector<uint8_t>{0,	  127, 0, 167, 10,	142, 47, 199, 23,  150, 107, 227, 38,  150, 174, 246,
								 34,  93,  0, 167, 51,	102, 47, 199, 69,  104, 107, 227, 88,  100, 174, 246,
								 68,  59,  0, 167, 91,	61,	 47, 199, 115, 58,	107, 227, 138, 50,	174, 246,
								 102, 25,  0, 167, 132, 20,	 47, 199, 162, 12,	107, 227, 188, 0,	174, 246},
			std::vector<uint8_t>{0,	 118, 0, 154, 10,  138, 45, 193, 24,  153, 109, 231, 38,  153, 177, 250,
								 31, 86,  0, 154, 49,  98,	45, 193, 71,  106, 109, 231, 89,  102, 177, 250,
								 63, 55,  0, 154, 89,  59,	45, 193, 118, 59,  109, 231, 140, 51,  177, 250,
								 94, 24,  0, 154, 128, 20,	45, 193, 165, 12,  109, 231, 191, 0,   177, 250},
			std::vector<uint8_t>{0,	 105, 0, 138, 10,  135, 44, 189, 24,  155, 110, 234, 39,  155, 179, 254,
								 28, 77,  0, 138, 48,  96,	44, 189, 72,  107, 110, 234, 91,  104, 179, 254,
								 56, 49,  0, 138, 87,  58,	44, 189, 119, 60,  110, 234, 142, 52,  179, 254,
								 84, 21,  0, 138, 125, 19,	44, 189, 167, 12,  110, 234, 194, 0,   179, 254},
			std::vector<uint8_t>{0,	 114, 0, 149, 10,  135, 44, 189, 24,  155, 110, 234, 39,  154, 178, 252,
								 30, 84,  0, 149, 48,  96,	44, 189, 72,  107, 110, 234, 90,  103, 178, 252,
								 61, 53,  0, 149, 87,  58,	44, 189, 119, 60,  110, 234, 141, 51,  178, 252,
								 91, 23,  0, 149, 125, 19,	44, 189, 167, 12,  110, 234, 193, 0,   178, 252},
			std::vector<uint8_t>{0,	 108, 0, 141, 10,  135, 44, 189, 24,  155, 110, 234, 39,  155, 179, 253,
								 29, 79,  0, 141, 48,  96,	44, 189, 72,  107, 110, 234, 90,  103, 179, 253,
								 58, 50,  0, 141, 87,  58,	44, 189, 119, 60,  110, 234, 142, 52,  179, 253,
								 86, 22,  0, 141, 125, 19,	44, 189, 167, 12,  110, 234, 194, 0,   179, 253}
		};
		Image i{4, 4, pixels[size_t(shape) + (distance ? 3 : 0)], 0};
		i.Hash = SurfaceHash(i);
		return i;
	}
}
TEST_CASE("Blobify all source shapes match independent literal pixels", "[source_blobify]") {
	for (int shape = 0; shape < 3; ++shape)
		Same(Draw(Blobify(shape)), Golden(shape));
}
TEST_CASE("Blobify source Distance weights match independent pixels", "[source_blobify]") {
	for (int shape = 0; shape < 3; ++shape) {
		auto d = Blobify(shape);
		Set(d, "distance", true);
		Same(Draw(d), Golden(shape, true));
	}
}
TEST_CASE("Blobify scalar inactive Active copies every shape", "[source_blobify]") {
	for (int shape = 0; shape < 3; ++shape) {
		auto d = Blobify(shape);
		Set(d, "active", false);
		Same(Draw(d), Source(d));
	}
}
TEST_CASE("Blobify no smoothness follows exact threshold step", "[source_blobify]") {
	auto d = Blobify(1);
	Set(d, "smoothness", 0.);
	Set(d, "threshold", 0.);
	Same(Draw(d), Source(d));
	Set(d, "threshold", 1.);
	auto image = Draw(d);
	for (auto n : image.Pixels)
		CHECK(n == 0);
}
TEST_CASE("Blobify Keep Alpha preserves alpha of rejected pixels", "[source_blobify]") {
	auto d = Blobify(2);
	Set(d, "threshold", 1.);
	Set(d, "smoothness", 0.);
	Set(d, "keep_alpha", true);
	auto image = Draw(d);
	for (size_t i = 0; i < image.Pixels.size(); ++i)
		CHECK(image.Pixels[i] == (i % 4 == 3 ? 255 : 0));
}
TEST_CASE("Blobify Circle radius zero names undefined empty accumulator", "[source_blobify]") {
	auto d = Blobify(0);
	Set(d, "radius", int64_t{0});
	Refuse(d, Status::UnsupportedExecution, "radius", Limits::MaximumEvaluationBytes, "undefined");
}
TEST_CASE("Blobify Square and Diamond radius zero sample the original pixel", "[source_blobify]") {
	for (int shape : {1, 2}) {
		auto d = Blobify(shape);
		Set(d, "radius", int64_t{0});
		Set(d, "threshold", 0.);
		Set(d, "smoothness", 0.);
		Same(Draw(d), Source(d));
	}
}
TEST_CASE("Blobify Distance zero radius refuses consumed zero division", "[source_blobify]") {
	for (int shape = 0; shape < 3; ++shape) {
		auto d = Blobify(shape);
		Set(d, "radius", int64_t{0});
		Set(d, "distance", true);
		Refuse(d, Status::UnsupportedExecution, "radius");
	}
}
TEST_CASE("Blobify negative smoothness refuses undefined source smoothstep edges", "[source_blobify]") {
	auto d = Blobify();
	Set(d, "smoothness", -.1);
	Refuse(d, Status::UnsupportedExecution, "smoothness");
}
TEST_CASE("Blobify mapped Radius missing surface uses validated low endpoint", "[source_blobify]") {
	auto d = Blobify(1);
	Erase(d, "radius");
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{1.5, 4.5});
	Same(Draw(d), Golden(1));
}
TEST_CASE("Blobify linked numeric Radius applies integer half even ties", "[source_blobify]") {
	for (const auto &[n, rounded] :
		 std::array{std::pair{1.5, int64_t{2}}, std::pair{2.5, int64_t{2}}, std::pair{3.5, int64_t{4}}}) {
		auto d = Blobify();
		d.Nodes.push_back({"number", "pc.number_simple", "", {}, {{"value", n}}});
		d.Links.push_back({"number", "number", "generator", "radius"});
		auto expected = Blobify();
		Set(expected, "radius", rounded);
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE("Blobify linked mapped Radius pair precedes synthetic endpoints", "[source_blobify]") {
	auto d = Blobify(2);
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{5, 7});
	d.Nodes.push_back(Array("r", ValueType::Scalar, 2.5, 3.5));
	d.Links.push_back({"r", "array", "generator", "radius"});
	Same(Draw(d), Golden(2));
}
TEST_CASE("Blobify white Radius map selects high endpoint ignoring map alpha", "[source_blobify]") {
	auto d = Blobify(1);
	Erase(d, "radius");
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{1, 2});
	d.Nodes.push_back(Solid("map", {4, 4}, {255, 255, 255, 0}));
	d.Links.push_back({"map", "surface_out", "generator", "radius_map"});
	Same(Draw(d), Golden(1));
}
TEST_CASE("Blobify unmapped Radius surface remains inert", "[source_blobify]") {
	auto d = Blobify();
	d.Nodes.push_back(Solid("map", {4, 4}, {0, 0, 0, 255}));
	d.Links.push_back({"map", "surface_out", "generator", "radius_map"});
	Same(Draw(d), Golden(0));
}
TEST_CASE("Blobify zero Mix and zero channel preserve original pixels", "[source_blobify]") {
	auto d = Blobify();
	Set(d, "mix", 0.);
	Same(Draw(d), Source(d));
	Set(d, "mix", 1.);
	Set(d, "channel", int64_t{0});
	Same(Draw(d), Source(d));
}
TEST_CASE("Blobify black mask preserves original and inverted mask applies kernel", "[source_blobify]") {
	auto d = Blobify();
	d.Nodes.push_back(Solid("mask", {4, 4}, {0, 0, 0, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	Same(Draw(d), Source(d));
	Set(d, "invert_mask", true);
	Same(Draw(d), Golden(0));
}
TEST_CASE("Blobify original later Radius is admitted before output", "[source_blobify]") {
	auto d = Blobify();
	d.Nodes.push_back(Array("r", ValueType::Integer, int64_t{1}, int64_t{10000}));
	d.Links.push_back({"r", "array", "generator", "radius"});
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array");
}
TEST_CASE("Blobify byte refusal preserves previous public Image", "[source_blobify]") {
	Refuse(Blobify(), Status::LimitExceeded, "", 1);
}
TEST_CASE("Blobify defined profiles round trip native document", "[source_blobify]") {
	for (int shape = 0; shape < 3; ++shape) {
		auto d = Blobify(shape);
		std::string saved;
		Diagnostic diag;
		saved = Write(d);
		Document copy;
		REQUIRE(Read(saved, copy, diag) == Status::Ok);
		Same(Draw(copy), Golden(shape));
	}
}
TEST_CASE(
	"Blobify animated integer Radius changes graph pixels at exact authored ticks", "[source_blobify]"
) {
	auto d = Blobify();
	d.Keyframes = {{"generator", "radius", 0, int64_t{1}}, {"generator", "radius", 2, int64_t{3}}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	for (uint64_t tick : {0, 2}) {
		EvaluationRequest request;
		request.Tick = tick;
		Image out;
		REQUIRE(Evaluate(d, plan, "out", request, out, diagnostic) == Status::Ok);
		auto expected = Blobify();
		Set(expected, "radius", int64_t(tick ? 3 : 1));
		Same(out, Draw(expected));
	}
}
TEST_CASE("Blobify later source pixels participate in whole array admission", "[source_blobify]") {
	auto d = Blobify();
	d.Nodes[1] = Solid("source", {1, 1}, {128, 64, 32, 255});
	d.Nodes.push_back(Solid("large", {256, 256}, {128, 64, 32, 255}));
	Node list{"list", "value.array", "", {}, {}};
	list.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(list);
	d.Links = {
		{"source", "surface_out", "list", "a"},
		{"large", "surface_out", "list", "b"},
		{"list", "array", "generator", "surface_in"}
	};
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("Blobify inactive first row still preflights later original input work", "[source_blobify]") {
	const auto *entry = FindCatalogueEntry("pc.blobify");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.blobify");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"generator", "pc.blobify", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext c(authored, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	const auto first = imagegraph_test::MakeImage(1, 1, {1, 2, 3, 255});
	ImageArray original;
	original.Images = {first, imagegraph_test::MakeImage(256, 256, std::vector<uint8_t>(256 * 256 * 4, 255))};
	c.Images = {{"surface_in", &first}};
	c.ImageArrays = {{"surface_in", &original}};
	c.ProcessorCount = 2;
	c.Values = {{"active", false}, {"radius", int64_t{2}}};
	CHECK_FALSE(executor(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.OutputImages.empty());
	CHECK(c.FailureMessage.find("whole-array work") != std::string::npos);
}

TEST_CASE(
	"Blobify accepted heterogeneous Radius rows retain independent scalar behavior", "[source_blobify]"
) {
	auto d = Blobify();
	d.Nodes.push_back(Array("radii", ValueType::Integer, int64_t{1}, int64_t{3}));
	d.Links.push_back({"radii", "array", "generator", "radius"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(d, plan, "out", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		auto expected = Blobify();
		Set(expected, "radius", int64_t(i ? 3 : 1));
		Same(rows.Images[i], Draw(expected));
	}
}
TEST_CASE("Blobify linked Surface Radius dimensions precede integer processing", "[source_blobify]") {
	auto d = Blobify();
	d.Nodes.push_back(Solid("dimension_source", {2, 3}, {255, 255, 255, 255}));
	d.Links.push_back({"dimension_source", "surface_out", "generator", "radius"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(d, plan, "out", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	Same(rows.Images[0], Golden(0));
	auto second = Blobify();
	Set(second, "radius", int64_t{3});
	Same(rows.Images[1], Draw(second));
}
TEST_CASE("Blobify output depth changes storage without replacing source input", "[source_blobify]") {
	for (int depth : {2, 3, 4, 5, 6, 7, 8}) {
		auto d = Blobify();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto out = Draw(d);
		CHECK(out.Format == *SourceSurfaceFormat(depth));
		CHECK(ValidSurfaceLayout(out, Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}
}
TEST_CASE("Blobify red safe draw bypasses the filter before mask finishing", "[source_blobify]") {
	for (int depth : {6, 7, 8}) {
		auto d = Blobify();
		for (auto &v : d.Nodes[1].Values)
			if (v.Port == "attribute_color_depth") v.Data = EnumValue{depth};
		Set(d, "attribute_color_depth", EnumValue{3});
		const auto source = Source(d), out = Draw(d);
		for (uint32_t y = 0; y < out.Height; ++y)
			for (uint32_t x = 0; x < out.Width; ++x) {
				SurfacePixel p;
				REQUIRE(LoadSurfacePixel(source, x, y, p));
				const size_t offset = (size_t(y) * out.Width + x) * 4;
				const uint8_t red = uint8_t(std::floor(std::clamp(p[0], 0., 1.) * 255 + .5));
				CHECK(out.Pixels[offset] == red);
				CHECK(out.Pixels[offset + 1] == red);
				CHECK(out.Pixels[offset + 2] == red);
				CHECK(out.Pixels[offset + 3] == 255);
			}
	}
}
TEST_CASE("Blobify inversion and Keep Alpha match independent source equation literal", "[source_blobify]") {
	auto d = Blobify(1);
	Set(d, "inverted", true);
	Set(d, "keep_alpha", true);
	Image expected{
		4,
		4,
		std::vector<uint8_t>{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
							 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
							 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
							 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
		0
	};
	expected.Hash = SurfaceHash(expected);
	Same(Draw(d), expected);
}
TEST_CASE("Blobify fractional mapped Radius retains continuous source Distance weight", "[source_blobify]") {
	auto d = Blobify();
	Erase(d, "radius");
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{2, 4});
	Set(d, "distance", true);
	d.Nodes.push_back(Solid("map", {1, 1}, {128, 128, 128, 0}));
	d.Links.push_back({"map", "surface_out", "generator", "radius_map"});
	Image expected{
		4,
		4,
		std::vector<uint8_t>{0,	 110, 0, 144, 10,  136, 45, 191, 24,  154, 109, 233, 39,  155, 178, 253,
							 29, 81,  0, 144, 49,  97,	45, 191, 71,  107, 109, 233, 90,  103, 178, 253,
							 59, 52,  0, 144, 88,  58,	45, 191, 119, 59,  109, 233, 142, 52,  178, 253,
							 88, 22,  0, 144, 127, 19,	45, 191, 166, 12,  109, 233, 193, 0,   178, 253},
		0
	};
	expected.Hash = SurfaceHash(expected);
	Same(Draw(d), expected);
}

TEST_CASE(
	"Blobify supported oversample modes follow independent edge sampling literals", "[source_blobify]"
) {
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{1});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0, 4, 0, 5,  1,  14, 5,  20, 2,  13, 9,  20, 1,  5,  6,  8,
								 3, 9, 0, 16, 14, 29, 13, 57, 17, 26, 27, 57, 9,  10, 17, 24,
								 7, 6, 0, 16, 26, 17, 13, 57, 29, 14, 27, 57, 13, 5,  17, 24,
								 3, 1, 0, 5,  13, 2,  5,  20, 14, 1,  9,  20, 6,  0,  6,  8},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{2});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	 28, 0, 37, 4,	53, 18, 75,	 8,	 49, 35, 75,  8,  33, 38, 54,
								 13, 35, 0, 62, 30, 60, 28, 119, 36, 54, 56, 119, 31, 36, 62, 88,
								 25, 22, 0, 62, 54, 36, 28, 119, 60, 30, 56, 119, 49, 18, 62, 88,
								 23, 6,	 0, 37, 49, 8,	18, 75,	 53, 4,	 35, 75,  41, 0,  38, 54},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{3});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	  127, 0, 167, 10,	142, 47, 199, 23,  150, 107, 227, 38,  150, 174, 246,
								 34,  93,  0, 167, 51,	102, 47, 199, 69,  104, 107, 227, 88,  100, 174, 246,
								 68,  59,  0, 167, 91,	61,	 47, 199, 115, 58,	107, 227, 138, 50,	174, 246,
								 102, 25,  0, 167, 132, 20,	 47, 199, 162, 12,	107, 227, 188, 0,	174, 246},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{4});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	  167, 0, 218, 12,	162, 53, 227, 20,  132, 94, 199, 32,  128, 148, 209,
								 44,  122, 0, 218, 58,	115, 53, 227, 61,  91,	94, 199, 75,  85,  148, 209,
								 89,  78,  0, 218, 104, 69,	 53, 227, 102, 51,	94, 199, 117, 43,  148, 209,
								 133, 33,  0, 218, 150, 23,	 53, 227, 142, 10,	94, 199, 160, 0,   148, 209},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{6});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	 36, 0, 47,	 3,	 36, 12, 50,  4,  27, 19, 40,  7,  27, 31, 44,
								 25, 69, 0, 122, 33, 66, 31, 130, 33, 49, 50, 107, 41, 47, 81, 115,
								 50, 44, 0, 122, 60, 40, 31, 130, 55, 27, 50, 107, 64, 23, 81, 115,
								 29, 7,	 0, 47,	 33, 5,	 12, 50,  29, 2,  19, 40,  33, 0,  31, 44},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{7});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	 85, 0, 111, 6,	 84, 28, 118, 10, 64, 46, 97,  16, 63, 73,	104,
								 34, 94, 0, 168, 45, 90, 42, 177, 46, 69, 70, 150, 57, 65, 112, 159,
								 69, 60, 0, 168, 81, 54, 42, 177, 76, 38, 70, 150, 89, 32, 112, 159,
								 68, 17, 0, 111, 78, 12, 28, 118, 69, 5,  46, 97,  79, 0,  73,	104},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{8});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	  167, 0, 218, 12,	162, 53, 227, 20,  132, 94, 199, 32,  128, 148, 209,
								 44,  122, 0, 218, 58,	115, 53, 227, 61,  91,	94, 199, 75,  85,  148, 209,
								 89,  78,  0, 218, 104, 69,	 53, 227, 102, 51,	94, 199, 117, 43,  148, 209,
								 133, 33,  0, 218, 150, 23,	 53, 227, 142, 10,	94, 199, 160, 0,   148, 209},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{10});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	 28, 0, 37, 6,	85, 28, 119, 12, 79, 56, 119, 8,  33, 38, 54,
								 8,	 21, 0, 37, 30, 60, 28, 119, 36, 54, 56, 119, 19, 22, 38, 54,
								 15, 13, 0, 37, 54, 36, 28, 119, 60, 30, 56, 119, 30, 11, 38, 54,
								 23, 6,	 0, 37, 79, 12, 28, 119, 85, 6,	 56, 119, 41, 0,  38, 54},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{11});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	 69, 0, 90, 8,	 117, 39, 164, 17,	109, 77, 164, 19, 77, 88, 125,
								 18, 50, 0, 90, 42,	 84,  39, 164, 50,	75,	 77, 164, 45, 51, 88, 125,
								 37, 32, 0, 90, 75,	 50,  39, 164, 84,	42,	 77, 164, 70, 26, 88, 125,
								 55, 14, 0, 90, 109, 17,  39, 164, 117, 8,	 77, 164, 96, 0,  88, 125},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
	{
		auto d = Blobify(1);
		Set(d, "oversample", EnumValue{12});
		Image expected{
			4,
			4,
			std::vector<uint8_t>{0,	  127, 0, 167, 10,	142, 47, 199, 23,  150, 107, 227, 38,  150, 174, 246,
								 34,  93,  0, 167, 51,	102, 47, 199, 69,  104, 107, 227, 88,  100, 174, 246,
								 68,  59,  0, 167, 91,	61,	 47, 199, 115, 58,	107, 227, 138, 50,	174, 246,
								 102, 25,  0, 167, 132, 20,	 47, 199, 162, 12,	107, 227, 188, 0,	174, 246},
			0
		};
		expected.Hash = SurfaceHash(expected);
		Same(Draw(d), expected);
	}
}
TEST_CASE("Blobify unmapped range metadata cannot impose unrelated work", "[source_blobify]") {
	auto d = Blobify(1);
	Set(d, "radius_map_range", Vector2{1e12, 1e12});
	Same(Draw(d), Golden(1));
}
TEST_CASE("Blobify ordinary polygon image admits only selected source loop work", "[source_blobify]") {
	auto d = Blobify(1);
	d.Nodes[1] = Solid("source", {64, 64}, {128, 64, 32, 255});
	Set(d, "radius", int64_t{3});
	Set(d, "threshold", 0.);
	Set(d, "smoothness", 0.);
	Same(Draw(d), Source(d));
}
TEST_CASE(
	"Blobify later Circle shape contributes angular work before first polygon row", "[source_blobify]"
) {
	auto d = Blobify(1);
	d.Nodes[1] = Solid("source", {64, 64}, {128, 64, 32, 255});
	Set(d, "radius", int64_t{3});
	d.Nodes.push_back(Array("shapes", ValueType::Enum, EnumValue{1}, EnumValue{0}));
	d.Links.push_back({"shapes", "array", "generator", "shape"});
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array");
}

TEST_CASE("Blobify numeric half-even radius is independent of ambient rounding mode", "[source_blobify]") {
	struct RestoreRound {
		int Original = std::fegetround();
		~RestoreRound() {
			std::fesetround(Original);
		}
	} restore;
	for (int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
		REQUIRE(std::fesetround(mode) == 0);
		for (const auto &[radius, rounded] :
			 std::array{std::pair{1.5, int64_t{2}}, std::pair{2.5, int64_t{2}}, std::pair{3.5, int64_t{4}}}) {
			auto linked = Blobify();
			linked.Nodes.push_back({"radius_source", "pc.number_simple", "", {}, {{"value", radius}}});
			linked.Links.push_back({"radius_source", "number", "generator", "radius"});
			auto expected = Blobify();
			Set(expected, "radius", rounded);
			Same(Draw(linked), Draw(expected));
		}
	}
}
TEST_CASE(
	"Blobify static toggle and sampler attributes cannot form later processor rows", "[source_blobify]"
) {
	const auto *entry = FindCatalogueEntry("pc.blobify");
	REQUIRE(entry);
	for (std::string port : {"radius_mapped", "interpolate"}) {
		const auto *input = FindCatalogueInput(*entry, port);
		REQUIRE(input);
		CHECK(input->SourceIndex == -1);
		const bool toggle = port == "radius_mapped";
		ArrayValue values = toggle ? ArrayValue{ValueType::Boolean, {false, true}}
								   : ArrayValue{ValueType::Enum, {EnumValue{1}, EnumValue{2}}};
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, values));
		auto authored = Blobify();
		Set(authored, port, values);
		Plan p;
		Diagnostic diag;
		CHECK(Compile(authored, p, diag) == Status::TypeMismatch);
		CHECK(diag.Port == port);
		auto linked = Blobify();
		linked.Nodes.push_back(
			toggle ? Array("attributes", ValueType::Boolean, false, true)
				   : Array("attributes", ValueType::Enum, EnumValue{1}, EnumValue{2})
		);
		linked.Links.push_back({"attributes", "array", "generator", port});
		REQUIRE(Compile(linked, p, diag) == Status::Ok);
		Image output{1, 1, {9, 8, 7, 6}, 0};
		const auto prior = output;
		CHECK(Evaluate(linked, p, "out", {}, output, diag) == Status::UnsupportedExecution);
		CHECK(diag.NodeId == "generator");
		CHECK(diag.Port == port);
		CHECK(diag.Message.find("reader cannot consume an array input") != std::string::npos);
		CHECK(output == prior);
	}
}
TEST_CASE("Blobify inert huge synthetic radius does not enlarge polygon work", "[source_blobify]") {
	auto d = Blobify(1);
	Set(d, "radius_map_range", Vector2{1e12, 1e12});
	Same(Draw(d), Golden(1));
}
TEST_CASE("Blobify mapped physical radius pair overrides huge synthetic work extent", "[source_blobify]") {
	auto d = Blobify(1);
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{1e12, 1e12});
	d.Nodes.push_back(Array("ranges", ValueType::Scalar, 2., 2.));
	d.Links.push_back({"ranges", "array", "generator", "radius"});
	Same(Draw(d), Golden(1));
}
