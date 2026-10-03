#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_shape_map")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Coordinates(uint32_t w = 5, uint32_t h = 5) {
		Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4), 0};
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x) {
				const size_t p = (size_t(y) * w + x) * 4;
				image.Pixels[p] = uint8_t(x * 40);
				image.Pixels[p + 1] = uint8_t(y * 40);
				image.Pixels[p + 3] = 128;
			}
		return image;
	}
	void Pixel(const Image &image, uint32_t x, uint32_t y, std::array<uint8_t, 4> expected) {
		REQUIRE(x < image.Width);
		REQUIRE(y < image.Height);
		const size_t p = (size_t(y) * image.Width + x) * 4;
		for (size_t c = 0; c < 4; ++c)
			CHECK(image.Pixels[p + c] == expected[c]);
	}
	Document Graph() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"map", "pc.shape_map", "", {}, {{"map_scale", Vector2{1, .75}}, {"interpolate", EnumValue{1}}}},
			{"consumer", "image.invert", "", {}, {{"include_alpha", false}}}
		};
		d.Links = {{"source", "image", "map", "surface_in"}, {"map", "surface_out", "consumer", "image"}};
		d.Outputs = {{"mapped", "map", "surface_out"}, {"out", "consumer", "image"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto status = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
}
TEST_CASE("Shape Map circle uses angle seam and radial repeat with raw alpha", "[source_2d][shape_map]") {
	auto source = Coordinates();
	auto r = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"map_scale", Vector2{1, .75}}, {"interpolate", EnumValue{1}}}
	);
	INFO(r.Message);
	REQUIRE(r.Ok);
	Pixel(r.Output(), 2, 2, {80, 0, 0, 128});
	Pixel(r.Output(), 3, 2, {80, 40, 0, 128});
	Pixel(r.Output(), 1, 2, {0, 40, 0, 128});
	Pixel(r.Output(), 2, 1, {40, 40, 0, 128});
	Pixel(r.Output(), 2, 3, {120, 40, 0, 128});
	Pixel(r.Output(), 0, 0, {0, 0, 0, 0});
}
TEST_CASE("Shape Map polygon uses sector half shift and triangle radius factor", "[source_2d][shape_map]") {
	auto source = Coordinates();
	auto square = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"shape", EnumValue{1}},
		 {"sides", int64_t{4}},
		 {"map_scale", Vector2{1, .75}},
		 {"interpolate", EnumValue{1}}}
	);
	REQUIRE(square.Ok);
	Pixel(square.Output(), 3, 2, {40, 40, 0, 128});
	Pixel(square.Output(), 2, 1, {120, 40, 0, 128});
	auto triangle = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"shape", EnumValue{1}},
		 {"sides", int64_t{3}},
		 {"map_scale", Vector2{1, .75}},
		 {"interpolate", EnumValue{1}}}
	);
	REQUIRE(triangle.Ok);
	CHECK(triangle.Output().Pixels[(2 * 5 + 4) * 4 + 3] == 128);
	CHECK(triangle.Output().Pixels[(2 * 5 + 0) * 4 + 3] == 0);
}
TEST_CASE(
	"Shape Map rotation and negative map scale retain source fract addressing", "[source_2d][shape_map]"
) {
	auto source = Coordinates();
	auto rotated = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"angle", 90.}, {"map_scale", Vector2{1, .75}}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(rotated.Ok);
	Pixel(rotated.Output(), 2, 1, {80, 40, 0, 128});
	auto reversed = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"map_scale", Vector2{-1, -.75}}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(reversed.Ok);
	Pixel(reversed.Output(), 3, 2, {80, 120, 0, 128});
}
TEST_CASE(
	"Shape Map unused Radius and Oversample controls do not affect wrapped mapping", "[source_2d][shape_map]"
) {
	auto source = Coordinates();
	auto a =
		RunNode("pc.shape_map", {{"surface_in", &source}}, {{"radius", -500.}, {"oversample", EnumValue{1}}});
	auto b =
		RunNode("pc.shape_map", {{"surface_in", &source}}, {{"radius", 500.}, {"oversample", EnumValue{12}}});
	REQUIRE(a.Ok);
	REQUIRE(b.Ok);
	CHECK(a.Output() == b.Output());
}
TEST_CASE("Shape Map negative circle scale keeps its one sided distance test", "[source_2d][shape_map]") {
	auto source = Coordinates();
	auto r = RunNode("pc.shape_map", {{"surface_in", &source}}, {{"scale", -1.}});
	REQUIRE(r.Ok);
	CHECK(r.Output().Pixels[3] == 128);
}
TEST_CASE(
	"Shape Map bilinear and bicubic use the source sampler coordinate convention", "[source_2d][shape_map]"
) {
	auto source = Coordinates();
	for (int64_t interpolation : {2, 3}) {
		auto r = RunNode(
			"pc.shape_map",
			{{"surface_in", &source}},
			{{"map_scale", Vector2{1, .65}}, {"interpolate", EnumValue{interpolation}}}
		);
		INFO(r.Message);
		REQUIRE(r.Ok);
		Pixel(r.Output(), 3, 2, {80, uint8_t(interpolation == 2 ? 32 : 36), 0, 128});
	}
}
TEST_CASE(
	"Shape Map Lanczos preserves a constant surface and diagnoses unported CleanEdge",
	"[source_2d][shape_map]"
) {
	auto source = Coordinates();
	for (size_t p = 0; p < source.Pixels.size(); p += 4) {
		source.Pixels[p] = 64;
		source.Pixels[p + 1] = 128;
		source.Pixels[p + 2] = 192;
	}
	auto r = RunNode("pc.shape_map", {{"surface_in", &source}}, {{"interpolate", EnumValue{4}}});
	INFO(r.Message);
	REQUIRE(r.Ok);
	Pixel(r.Output(), 3, 2, {64, 128, 192, 128});
	auto refused = RunNode("pc.shape_map", {{"surface_in", &source}}, {{"interpolate", EnumValue{6}}});
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::UnsupportedExecution);
	CHECK(refused.Message.find("CleanEdge") != std::string::npos);
}
TEST_CASE(
	"Shape Map inactive copy bypasses undefined active divisors and preserves input format",
	"[source_2d][shape_map]"
) {
	auto source = Coordinates();
	auto r = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"active", false}, {"scale", 0.}, {"shape", EnumValue{1}}, {"sides", int64_t{0}}}
	);
	REQUIRE(r.Ok);
	CHECK(r.Output().Pixels == source.Pixels);
	CHECK(r.Output().Format == source.Format);
}
TEST_CASE("Shape Map single red safe draw replaces the mapping shader", "[source_2d][shape_map]") {
	Image source{2, 2, {32, 64, 96, 128}, 0, SurfaceFormat::R8Unorm};
	auto r = RunNode(
		"pc.shape_map", {{"surface_in", &source}}, {{"scale", 0.}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(r.Ok);
	CHECK(r.Output().Format == SurfaceFormat::RGBA8Unorm);
	Pixel(r.Output(), 0, 0, {32, 32, 32, 255});
	Pixel(r.Output(), 1, 1, {128, 128, 128, 255});
}
TEST_CASE("Shape Map persisted graph feeds a real selected invert consumer", "[source_2d][shape_map]") {
	auto d = Graph();
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	auto p = Compiled(restored);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image;
	REQUIRE(Evaluate(restored, p, "out", request, image, diag) == Status::Ok);
	Pixel(image, 3, 2, {175, 215, 255, 128});
	Pixel(image, 0, 0, {255, 255, 255, 0});
}
TEST_CASE("Shape Map compiled array controls retain whole source image rows", "[source_2d][shape_map]") {
	auto d = Graph();
	d.Nodes.erase(d.Nodes.begin());
	d.Nodes.insert(
		d.Nodes.begin(),
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension_unit", EnumValue{0}}, {"color", Colour{64, 128, 192, 128}}}}
	);
	ArrayValue dimensions{ValueType::Vector2, {Vector2{3, 3}, Vector2{5, 5}}};
	d.Junctions = {{"sizes", "", ValueType::Array, dimensions}};
	d.Links = {
		{"sizes", "value", "solid", "dimension"},
		{"solid", "surface_out", "map", "surface_in"},
		{"map", "surface_out", "consumer", "image"}
	};
	d.Outputs = {{"mapped", "map", "surface_out"}};
	auto p = Compiled(d);
	ImageArray images;
	Diagnostic diag;
	REQUIRE(EvaluateArray(d, p, "mapped", {}, images, diag) == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Width == 3);
	CHECK(images.Images[1].Width == 5);
	Pixel(images.Images[0], 1, 1, {64, 128, 192, 128});
	Pixel(images.Images[1], 2, 2, {64, 128, 192, 128});
}
TEST_CASE(
	"Shape Map complete original batch work precedes tight output byte admission", "[source_2d][shape_map]"
) {
	auto source = Coordinates(2, 2);
	ImageArray original;
	original.Images = {source, Image{1024, 1024, std::vector<uint8_t>(1024 * 1024 * 4), 0}};
	const auto *entry = FindCatalogueEntry("pc.shape_map");
	REQUIRE(entry != nullptr);
	Node node{"map", "pc.shape_map", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.ProcessorCount = 2;
	context.Images = {{"surface_in", &source}};
	context.ImageArrays = {{"surface_in", &original}};
	const auto execute = detail::FindExecutor("pc.shape_map");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(context.OutputImages.empty());
}
TEST_CASE("Shape Map undefined coordinates preserve prior compiled publication", "[source_2d][shape_map]") {
	auto d = Graph();
	d.Nodes[1].Values.push_back({"scale", 0.});
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "mapped", request, image, diag) == Status::InvalidValue);
	CHECK(diag.Port == "scale");
	CHECK(image == prior);
}
TEST_CASE("Shape Map publication byte refusal preserves existing selected image", "[source_2d][shape_map]") {
	auto d = Graph();
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "mapped", request, image, diag, 1) == Status::LimitExceeded);
	CHECK(image == prior);
}
TEST_CASE(
	"Shape Map animated inherited controls resolve independently on repeated ticks", "[source_2d][shape_map]"
) {
	auto d = Graph();
	d.Keyframes = {{"map", "angle", 0, 0.}, {"map", "angle", 12, 90.}};
	d.Nodes[1].SourceAnimatedInputs = {"angle"};
	Node instance{"instance", "pc.shape_map", "", {}, {}};
	instance.InstanceBase = "map";
	d.Nodes.push_back(instance);
	d.Outputs = {{"mapped", "instance", "surface_out"}};
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Diagnostic diag;
	Image first, later, again;
	REQUIRE(Evaluate(d, p, "mapped", request, first, diag) == Status::Ok);
	request.Tick = 12;
	REQUIRE(Evaluate(d, p, "mapped", request, later, diag) == Status::Ok);
	Pixel(first, 2, 1, {40, 40, 0, 128});
	Pixel(later, 2, 1, {80, 40, 0, 128});
	request.Tick = 0;
	REQUIRE(Evaluate(d, p, "mapped", request, again, diag) == Status::Ok);
	CHECK(first == again);
}
TEST_CASE(
	"Shape Map float input and explicit byte output retain distinct numeric depth behavior",
	"[source_2d][shape_map]"
) {
	Image source{5, 5, std::vector<uint8_t>(5 * 5 * 16), 0, SurfaceFormat::RGBA32Float};
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 5; ++x)
			REQUIRE(StoreSurfacePixel(source, x, y, {2, -1, .25, .5}));
	auto same = RunNode("pc.shape_map", {{"surface_in", &source}}, {{"attribute_color_depth", EnumValue{0}}});
	REQUIRE(same.Ok);
	CHECK(same.Output().Format == SurfaceFormat::RGBA32Float);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(same.Output(), 3, 2, pixel));
	CHECK(pixel == SurfacePixel{2, -1, .25, .5});
	auto bytes =
		RunNode("pc.shape_map", {{"surface_in", &source}}, {{"attribute_color_depth", EnumValue{3}}});
	REQUIRE(bytes.Ok);
	Pixel(bytes.Output(), 3, 2, {255, 0, 64, 128});
}
TEST_CASE(
	"Shape Map finite control overflow refuses before undefined texel conversion", "[source_2d][shape_map]"
) {
	auto source = Coordinates();
	auto r = RunNode(
		"pc.shape_map",
		{{"surface_in", &source}},
		{{"shape", EnumValue{1}}, {"angle", 1.7e308}, {"map_scale", Vector2{1.7e308, 1.7e308}}}
	);
	CHECK_FALSE(r.Ok);
	CHECK(r.Code == Status::InvalidValue);
	CHECK(r.Message.find("coordinate") != std::string::npos);
}
TEST_CASE(
	"Shape Map compiled expensive second image refuses the whole batch atomically", "[source_2d][shape_map]"
) {
	auto d = Graph();
	d.Nodes.erase(d.Nodes.begin());
	d.Nodes.insert(
		d.Nodes.begin(),
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension_unit", EnumValue{0}}, {"color", Colour{64, 128, 192, 128}}}}
	);
	ArrayValue dimensions{ValueType::Vector2, {Vector2{2, 2}, Vector2{1024, 1024}}};
	d.Junctions = {{"sizes", "", ValueType::Array, dimensions}};
	d.Links = {
		{"sizes", "value", "solid", "dimension"},
		{"solid", "surface_out", "map", "surface_in"},
		{"map", "surface_out", "consumer", "image"}
	};
	d.Outputs = {{"mapped", "map", "surface_out"}};
	auto p = Compiled(d);
	ImageArray images;
	images.Images = {{1, 1, {9, 8, 7, 6}, 0}};
	const auto prior = images;
	Diagnostic diag;
	CHECK(EvaluateArray(d, p, "mapped", {}, images, diag) == Status::LimitExceeded);
	CHECK(diag.NodeId == "map");
	CHECK(diag.Message.find("complete processor batch") != std::string::npos);
	CHECK(images.Images == prior.Images);
	CHECK(images.Items == prior.Items);
}
TEST_CASE(
	"Shape Map preflights retained original Atlas rows before selected surface projection",
	"[source_2d][shape_map]"
) {
	auto source = Coordinates(2, 2);
	AtlasValue atlas;
	auto &payload = atlas.Data.emplace();
	payload.Kind = AtlasKind::SurfaceAtlas;
	payload.Surface.Data = {800, 800, std::vector<uint8_t>(800 * 800 * 4), 0};
	payload.Dimension = {800, 800};
	payload.OriginalDimension = {800, 800};
	AtlasValue small;
	auto &smallPayload = small.Data.emplace();
	smallPayload.Kind = AtlasKind::SurfaceAtlas;
	smallPayload.Surface.Data = source;
	smallPayload.Dimension = {2, 2};
	smallPayload.OriginalDimension = {2, 2};
	Value original = ArrayValue{ValueType::Atlas, {small, atlas}};
	const std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"surface_in", &original}}};
	const auto *entry = FindCatalogueEntry("pc.shape_map");
	REQUIRE(entry != nullptr);
	Node node{"map", "pc.shape_map", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.ProcessorCount = 2;
	context.ProcessorOriginalValues = originals;
	context.Images = {{"surface_in", &source}};
	const auto execute = detail::FindExecutor("pc.shape_map");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(context));
	CHECK(context.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(context.OutputImages.empty());
}
