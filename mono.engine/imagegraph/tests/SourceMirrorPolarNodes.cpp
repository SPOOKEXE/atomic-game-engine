#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_mirror_polar")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Coordinates(uint32_t width = 8, uint32_t height = 8) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const size_t index = (size_t(y) * width + x) * 4;
				image.Pixels[index] = uint8_t(x * 20);
				image.Pixels[index + 1] = uint8_t(y * 20);
				image.Pixels[index + 3] = 128;
			}
		return image;
	}
	void Pixel(const Image &image, uint32_t x, uint32_t y, std::array<uint8_t, 4> expected) {
		REQUIRE(image.Format == SurfaceFormat::RGBA8Unorm);
		REQUIRE(x < image.Width);
		REQUIRE(y < image.Height);
		const size_t index = (size_t(y) * image.Width + x) * 4;
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(image.Pixels[index + channel] == expected[channel]);
	}
	Document Graph() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"polar", "pc.mirror_polar", "", {}, {{"interpolate", EnumValue{1}}}},
			{"consumer", "image.invert", "", {}, {{"include_alpha", false}}}
		};
		d.Links = {{"source", "image", "polar", "surface_in"}, {"polar", "surface_out", "consumer", "image"}};
		d.Outputs = {{"mapped", "polar", "surface_out"}, {"out", "consumer", "image"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan plan;
		Diagnostic diag;
		const auto status = Compile(d, plan, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
}
TEST_CASE("Polar Mirror folds ordinary and reflective sectors independently", "[source_2d][mirror_polar]") {
	auto source = Coordinates();
	auto ordinary = RunNode("pc.mirror_polar", {{"surface_in", &source}}, {{"interpolate", EnumValue{1}}});
	INFO(ordinary.Message);
	REQUIRE(ordinary.Ok);
	Pixel(ordinary.Output(), 5, 4, {80, 100, 0, 128});
	Pixel(ordinary.Output(), 4, 3, {80, 80, 0, 128});
	auto reflected = RunNode(
		"pc.mirror_polar", {{"surface_in", &source}}, {{"reflective", true}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(reflected.Ok);
	Pixel(reflected.Output(), 5, 4, {100, 80, 0, 128});
}
TEST_CASE("Polar Mirror rotation is the source row-vector matrix direction", "[source_2d][mirror_polar]") {
	auto source = Coordinates();
	auto before = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"center", Vector2{4.25, 4.25}},
		 {"center_unit", EnumValue{0}},
		 {"spokes", 3.},
		 {"interpolate", EnumValue{1}}}
	);
	auto after = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"center", Vector2{4.25, 4.25}},
		 {"center_unit", EnumValue{0}},
		 {"rotation", 90.},
		 {"spokes", 3.},
		 {"interpolate", EnumValue{1}}}
	);
	REQUIRE(before.Ok);
	REQUIRE(after.Ok);
	Pixel(before.Output(), 5, 4, {60, 100, 0, 128});
	Pixel(after.Output(), 5, 4, {100, 80, 0, 128});
}
TEST_CASE(
	"Polar Mirror angle shift and negative angular scale preserve wrapped coordinates",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	auto angle = RunNode(
		"pc.mirror_polar", {{"surface_in", &source}}, {{"angle", 90.}, {"interpolate", EnumValue{1}}}
	);
	auto negative = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"scale", Vector2{-1, 1}}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(angle.Ok);
	REQUIRE(negative.Ok);
	Pixel(angle.Output(), 5, 4, {100, 60, 0, 128});
	Pixel(negative.Output(), 5, 4, {80, 40, 0, 128});
}
TEST_CASE(
	"Polar Mirror exponential radius and post-scale trim retain source ordering", "[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	auto squared = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"radial_scale", EnumValue{1}}, {"scale", Vector2{1, 2}}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(squared.Ok);
	Pixel(squared.Output(), 5, 4, {80, 120, 0, 128});
	auto trim = RunNode(
		"pc.mirror_polar", {{"surface_in", &source}}, {{"trim_radius", .3}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(trim.Ok);
	Pixel(trim.Output(), 5, 4, {0, 0, 0, 0});
	Pixel(trim.Output(), 4, 3, {80, 80, 0, 128});
}
TEST_CASE(
	"Polar Mirror Reference center uses raw fractional output size before rounding",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"output_dimension", EnumValue{1}},
		 {"relative_dimension", Vector2{.53125, .53125}},
		 {"center", Vector2{.6, .6}},
		 {"scale", Vector2{0, 0}},
		 {"interpolate", EnumValue{1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 4);
	CHECK(run.Output().Height == 4);
	Pixel(run.Output(), 0, 0, {100, 100, 0, 128});
}
TEST_CASE("Polar Mirror persisted numeric center link keeps Reference units", "[source_2d][mirror_polar]") {
	auto d = Graph();
	d.Nodes[1].Values.insert(
		d.Nodes[1].Values.end(),
		{{"output_dimension", EnumValue{1}},
		 {"relative_dimension", Vector2{.53125, .53125}},
		 {"scale", Vector2{0, 0}}}
	);
	d.Junctions = {{"center", "", ValueType::Vector2, Vector2{.6, .6}}};
	d.Links.push_back({"center", "value", "polar", "center"});
	const std::string text = Write(d);
	REQUIRE_FALSE(text.empty());
	Diagnostic diag;
	Document loaded;
	REQUIRE(Read(text, loaded, diag) == Status::Ok);
	auto p = Compiled(loaded);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image;
	REQUIRE(Evaluate(loaded, p, "out", request, image, diag) == Status::Ok);
	Pixel(image, 0, 0, {155, 155, 255, 128});
}
TEST_CASE("Polar Mirror linked surfaces supply physical center dimensions", "[source_2d][mirror_polar]") {
	auto d = Graph();
	d.Nodes.push_back(
		{"center-image", "pc.solid", "", {}, {{"dimension", Vector2{2, 3}}, {"dimension_unit", EnumValue{0}}}}
	);
	d.Nodes[1].Values.push_back({"scale", Vector2{0, 0}});
	d.Links.push_back({"center-image", "surface_out", "polar", "center"});
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image;
	Diagnostic diag;
	REQUIRE(Evaluate(d, p, "mapped", request, image, diag) == Status::Ok);
	Pixel(image, 0, 0, {40, 60, 0, 128});
}
TEST_CASE(
	"Polar Mirror mapped spoke range is one control and ignores map alpha", "[source_2d][mirror_polar]"
) {
	auto d = Graph();
	d.Nodes[1].Values.push_back({"spokes_mapped", true});
	d.Nodes.push_back(
		{"map",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{0, 0, 0, 0}}}}
	);
	d.Junctions = {{"spokes", "", ValueType::Array, ArrayValue{ValueType::Scalar, {2., 4.}}}};
	d.Links.push_back({"spokes", "value", "polar", "spokes"});
	d.Links.push_back({"map", "surface_out", "polar", "spokes_map"});
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image;
	Diagnostic diag;
	REQUIRE(Evaluate(d, p, "mapped", request, image, diag) == Status::Ok);
	Pixel(image, 5, 4, {40, 80, 0, 128});
	d.Nodes.back().Values.back().Data = Colour{255, 255, 255, 0};
	p = Compiled(d);
	REQUIRE(Evaluate(d, p, "mapped", request, image, diag) == Status::Ok);
	Pixel(image, 5, 4, {80, 100, 0, 128});
}
TEST_CASE(
	"Polar Mirror source shader curve scales spokes by normalized radial distance",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	Curve curve;
	curve.Header = {0, 1, 0, .5, .5, 0};
	curve.Anchors = {{0, 0, 0, 0, 1. / 3, 1. / 3}, {-1. / 3, -1. / 3, 1, 1, 0, 0}};
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"spokes_curved", true}, {"spokes_curve", curve}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(run.Ok);
	Pixel(run.Output(), 5, 4, {40, 80, 0, 128});
	curve.Header[1] = 0;
	auto bad = RunNode(
		"pc.mirror_polar", {{"surface_in", &source}}, {{"spokes_curved", true}, {"spokes_curve", curve}}
	);
	CHECK_FALSE(bad.Ok);
	CHECK(bad.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Polar Mirror sampling modes preserve constant alpha and oversample is inert after fract",
	"[source_2d][mirror_polar]"
) {
	Image source{8, 8, std::vector<uint8_t>(8 * 8 * 4), 0};
	for (size_t i = 0; i < source.Pixels.size(); i += 4) {
		source.Pixels[i] = 64;
		source.Pixels[i + 1] = 128;
		source.Pixels[i + 2] = 192;
		source.Pixels[i + 3] = 128;
	}
	for (const int64_t mode : {1, 2, 3, 4}) {
		auto run = RunNode(
			"pc.mirror_polar",
			{{"surface_in", &source}},
			{{"interpolate", EnumValue{mode}}, {"oversample", EnumValue{1}}}
		);
		INFO(mode);
		INFO(run.Message);
		REQUIRE(run.Ok);
		Pixel(run.Output(), 5, 4, {64, 128, 192, 128});
	}
	auto edge = RunNode("pc.mirror_polar", {{"surface_in", &source}}, {{"interpolate", EnumValue{6}}});
	REQUIRE(edge.Ok);
	Pixel(edge.Output(), 5, 4, {64, 128, 192, 128});
}
TEST_CASE(
	"Polar Mirror inactive clone ignores output dimensions and undefined spokes", "[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"active", false},
		 {"output_dimension", EnumValue{2}},
		 {"constant_dimension", Vector2{1e300, 1e300}},
		 {"spokes", 0.},
		 {"interpolate", EnumValue{6}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output() == source);
}
TEST_CASE("Polar Mirror safe red source stretches without the polar kernel", "[source_2d][mirror_polar]") {
	Image source{2, 1, {64, 192}, 0, SurfaceFormat::R8Unorm};
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"output_dimension", EnumValue{2}},
		 {"constant_dimension", Vector2{4, 2}},
		 {"spokes", 0.},
		 {"scale", Vector2{0, 0}},
		 {"interpolate", EnumValue{6}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	Pixel(run.Output(), 0, 0, {64, 64, 64, 255});
	Pixel(run.Output(), 3, 1, {192, 192, 192, 255});
}
TEST_CASE(
	"Polar Mirror source float depth and normalized output stay distinct", "[source_2d][mirror_polar]"
) {
	Image source{8, 8, std::vector<uint8_t>(8 * 8 * 16), 0, SurfaceFormat::RGBA32Float};
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x)
			REQUIRE(StoreSurfacePixel(source, x, y, {2, -1, .25, .5}));
	auto floating = RunNode("pc.mirror_polar", {{"surface_in", &source}});
	REQUIRE(floating.Ok);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(floating.Output(), 5, 4, pixel));
	CHECK(pixel == SurfacePixel{2, -1, .25, .5});
	auto bytes =
		RunNode("pc.mirror_polar", {{"surface_in", &source}}, {{"attribute_color_depth", EnumValue{3}}});
	REQUIRE(bytes.Ok);
	Pixel(bytes.Output(), 5, 4, {255, 0, 64, 128});
}
TEST_CASE(
	"Polar Mirror compiled image arrays retain dimensions and use first raw Reference size",
	"[source_2d][mirror_polar]"
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
	d.Junctions = {
		{"sizes", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{3, 3}, Vector2{5, 5}}}}
	};
	d.Links = {
		{"sizes", "value", "solid", "dimension"},
		{"solid", "surface_out", "polar", "surface_in"},
		{"polar", "surface_out", "consumer", "image"}
	};
	d.Outputs = {{"mapped", "polar", "surface_out"}};
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
	"Polar Mirror inherited animated instance replays identical selected pixels", "[source_2d][mirror_polar]"
) {
	auto d = Graph();
	d.Nodes[1].Values.insert(
		d.Nodes[1].Values.end(),
		{{"center", Vector2{4.25, 4.25}}, {"center_unit", EnumValue{0}}, {"spokes", 3.}}
	);
	d.Keyframes = {{"polar", "rotation", 0, 0.}, {"polar", "rotation", 12, 90.}};
	d.Nodes[1].SourceAnimatedInputs = {"rotation"};
	Node instance{"instance", "pc.mirror_polar", "", {}, {}};
	instance.InstanceBase = "polar";
	d.Nodes.push_back(instance);
	d.Outputs = {{"mapped", "instance", "surface_out"}};
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image first, later, again;
	Diagnostic diag;
	REQUIRE(Evaluate(d, p, "mapped", request, first, diag) == Status::Ok);
	request.Tick = 12;
	REQUIRE(Evaluate(d, p, "mapped", request, later, diag) == Status::Ok);
	Pixel(first, 5, 4, {60, 100, 0, 128});
	Pixel(later, 5, 4, {100, 80, 0, 128});
	request.Tick = 0;
	REQUIRE(Evaluate(d, p, "mapped", request, again, diag) == Status::Ok);
	CHECK(again == first);
}
TEST_CASE(
	"Polar Mirror undefined spoke divisor preserves prior compiled publication", "[source_2d][mirror_polar]"
) {
	auto d = Graph();
	d.Nodes[1].Values.push_back({"spokes", 0.});
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "mapped", request, image, diag) == Status::InvalidValue);
	CHECK(diag.Port == "spokes");
	CHECK(image == prior);
}
TEST_CASE(
	"Polar Mirror finite controls refuse undefined coordinates before sampler casts",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"center", Vector2{1e308, 1e308}}, {"center_unit", EnumValue{0}}}
	);
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::InvalidValue);
	auto tiny = RunNode(
		"pc.mirror_polar", {{"surface_in", &source}}, {{"spokes", std::numeric_limits<double>::denorm_min()}}
	);
	CHECK_FALSE(tiny.Ok);
	CHECK(tiny.Port == "spokes");
}
TEST_CASE(
	"Polar Mirror worst original relative size refuses before tight output bytes", "[source_2d][mirror_polar]"
) {
	auto source = Coordinates(2, 2);
	const auto *entry = FindCatalogueEntry("pc.mirror_polar");
	REQUIRE(entry != nullptr);
	Node node{"polar", "pc.mirror_polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = 1;
	c.ProcessorCount = 2;
	c.Images = {{"surface_in", &source}};
	c.Values = {{"output_dimension", EnumValue{1}}, {"relative_dimension", Vector2{1, 1}}};
	const Value original = ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{512, 512}}};
	const std::array<std::pair<std::string_view, const Value *>, 1> originals{
		{{"relative_dimension", &original}}
	};
	c.ProcessorOriginalValues = originals;
	const auto execute = detail::FindExecutor("pc.mirror_polar");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(c.OutputImages.empty());
}
TEST_CASE(
	"Polar Mirror compiled expensive second relative control refuses atomically", "[source_2d][mirror_polar]"
) {
	auto d = Graph();
	d.Nodes[1].Values.push_back({"output_dimension", EnumValue{1}});
	d.Junctions = {
		{"relative", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{128, 128}}}}
	};
	d.Links.push_back({"relative", "value", "polar", "relative_dimension"});
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray images;
	images.Images = {{1, 1, {9, 8, 7, 6}, 0}};
	const auto prior = images;
	Diagnostic diag;
	CHECK(EvaluateArray(d, p, "mapped", request, images, diag) == Status::LimitExceeded);
	CHECK(diag.NodeId == "polar");
	CHECK(diag.Message.find("complete processor batch") != std::string::npos);
	CHECK(images.Images == prior.Images);
	CHECK(images.Items == prior.Items);
}
TEST_CASE(
	"Polar Mirror output byte admission preserves previous selected image", "[source_2d][mirror_polar]"
) {
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
	"Polar Mirror prepared Reference size follows first image topology rather than selected row",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	ImageArray original;
	original.Images = {source, Coordinates(3, 3)};
	original.Items = {ImageArrayItem{size_t{1}}, ImageArrayItem{size_t{0}}};
	const auto *entry = FindCatalogueEntry("pc.mirror_polar");
	REQUIRE(entry != nullptr);
	Node node{"polar", "pc.mirror_polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	c.ProcessorRow = 1;
	c.ProcessorCount = 2;
	c.Images = {{"surface_in", &source}};
	c.ImageArrays = {{"surface_in", &original}};
	c.Values = {{"center", Vector2{.6, .6}}, {"scale", Vector2{0, 0}}, {"interpolate", EnumValue{1}}};
	const auto execute = detail::FindExecutor("pc.mirror_polar");
	REQUIRE(execute != nullptr);
	const bool ok = execute(c);
	INFO(c.FailureMessage);
	REQUIRE(ok);
	REQUIRE(c.OutputImages.size() == 1);
	Pixel(c.OutputImages.front().second, 0, 0, {20, 20, 0, 128});
}
TEST_CASE(
	"Polar Mirror whole batch includes later active rows before an inactive first clone",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	const auto *entry = FindCatalogueEntry("pc.mirror_polar");
	REQUIRE(entry != nullptr);
	Node node{"polar", "pc.mirror_polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = 1;
	c.ProcessorCount = 2;
	c.Images = {{"surface_in", &source}};
	c.Values = {
		{"active", false}, {"output_dimension", EnumValue{2}}, {"constant_dimension", Vector2{1024, 1024}}
	};
	const Value active = ArrayValue{ValueType::Boolean, {false, true}};
	const std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"active", &active}}};
	c.ProcessorOriginalValues = originals;
	const auto execute = detail::FindExecutor("pc.mirror_polar");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(c.OutputImages.empty());
}
TEST_CASE(
	"Polar Mirror fresh Constant dimension resolves actual project controls", "[source_2d][mirror_polar]"
) {
	auto d = Graph();
	d.Project.emplace();
	d.Project->SurfaceWidth = 7;
	d.Project->SurfaceHeight = 5;
	d.Nodes[1].Values.push_back({"output_dimension", EnumValue{2}});
	auto p = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image image;
	Diagnostic diag;
	REQUIRE(Evaluate(d, p, "mapped", request, image, diag) == Status::Ok);
	CHECK(image.Width == 7);
	CHECK(image.Height == 5);
}
TEST_CASE(
	"Polar Mirror authored original control array remains visible beneath selected row views",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates(2, 2);
	const auto *entry = FindCatalogueEntry("pc.mirror_polar");
	REQUIRE(entry != nullptr);
	Node node{"polar", "pc.mirror_polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = 1;
	c.ProcessorCount = 2;
	c.Images = {{"surface_in", &source}};
	c.Values = {
		{"output_dimension", EnumValue{1}},
		{"relative_dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{512, 512}}}}
	};
	const Value selected = Vector2{1, 1};
	c.ValueViews = {{"relative_dimension", &selected}};
	const auto execute = detail::FindExecutor("pc.mirror_polar");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(c.OutputImages.empty());
}
TEST_CASE(
	"Polar Mirror physical position changes folding without changing the sampling center",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"position", Vector2{1, 0}}, {"position_unit", EnumValue{0}}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(run.Ok);
	Pixel(run.Output(), 5, 4, {80, 80, 0, 128});
}
TEST_CASE(
	"Polar Mirror zero-radius negative exponential refuses undefined distance", "[source_2d][mirror_polar]"
) {
	auto source = Coordinates(3, 3);
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"radial_scale", EnumValue{1}}, {"scale", Vector2{1, -1}}, {"interpolate", EnumValue{1}}}
	);
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::InvalidValue);
	CHECK(run.Port == "scale");
}
TEST_CASE(
	"Polar Mirror unobserved path vector getter refuses instead of defaulting a center",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	Path2D path;
	path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{1, 1, 0, 0, 0, 0}, 0}};
	auto run = RunNode("pc.mirror_polar", {{"surface_in", &source}}, {{"center", path}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::UnsupportedExecution);
	CHECK(run.Port == "center");
}
TEST_CASE(
	"Polar Mirror preflights retained Atlas dimensions beneath a selected surface",
	"[source_2d][mirror_polar]"
) {
	auto source = Coordinates(2, 2);
	AtlasValue large;
	auto &data = large.Data.emplace();
	data.Kind = AtlasKind::SurfaceAtlas;
	data.Surface.Data = {800, 800, std::vector<uint8_t>(800 * 800 * 4), 0};
	data.Dimension = {800, 800};
	data.OriginalDimension = data.Dimension;
	const Value original = ArrayValue{ValueType::Atlas, {large}};
	const std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"surface_in", &original}}};
	const auto *entry = FindCatalogueEntry("pc.mirror_polar");
	REQUIRE(entry != nullptr);
	Node node{"polar", "pc.mirror_polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = 1;
	c.ProcessorCount = 2;
	c.Images = {{"surface_in", &source}};
	c.ProcessorOriginalValues = originals;
	const auto execute = detail::FindExecutor("pc.mirror_polar");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(c.OutputImages.empty());
}
TEST_CASE(
	"Polar Mirror scalar vector getters broadcast physical numeric leaves", "[source_2d][mirror_polar]"
) {
	auto source = Coordinates();
	const std::array<Value, 3> centers{{Value{2.}, Value{int64_t{2}}, Value{true}}};
	for (size_t index = 0; index < centers.size(); ++index) {
		auto run = RunNode(
			"pc.mirror_polar",
			{{"surface_in", &source}},
			{{"center", centers[index]},
			 {"center_unit", EnumValue{0}},
			 {"scale", Vector2{0, 0}},
			 {"interpolate", EnumValue{1}}}
		);
		REQUIRE(run.Ok);
		const uint8_t expected = index == 2 ? 20 : 40;
		Pixel(run.Output(), 0, 0, {expected, expected, 0, 128});
	}
}
TEST_CASE(
	"Polar Mirror CleanEdge keeps pinned center texel when no neighboring colors match",
	"[source_2d][mirror_polar]"
) {
	const auto source = Coordinates();
	auto run = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"interpolate", EnumValue{6}}, {"spokes", 1.}, {"center", Vector2{}}, {"center_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	// Unique neighbors reject all slices. ceil(2.5,4.5)/(8+.0001) nearest reads select(2,4).
	Pixel(run.Output(), 2, 3, {40, 80, 0, 128});
	const auto nearest = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"interpolate", EnumValue{1}}, {"spokes", 1.}, {"center", Vector2{}}, {"center_unit", EnumValue{0}}}
	);
	REQUIRE(nearest.Ok);
	Pixel(nearest.Output(), 2, 3, {40, 80, 0, 128});
	CHECK(nearest.Output() == run.Output());
	auto oversample = RunNode(
		"pc.mirror_polar",
		{{"surface_in", &source}},
		{{"interpolate", EnumValue{6}},
		 {"oversample", EnumValue{2}},
		 {"spokes", 1.},
		 {"center", Vector2{}},
		 {"center_unit", EnumValue{0}}}
	);
	REQUIRE(oversample.Ok);
	CHECK(oversample.Output() == run.Output());
}
TEST_CASE(
	"Polar Mirror CleanEdge preserves floating RGBA and bounded compiled publication",
	"[source_2d][mirror_polar]"
) {
	Image source{8, 8, std::vector<uint8_t>(8 * 8 * 16), 0, SurfaceFormat::RGBA32Float};
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x)
			REQUIRE(StoreSurfacePixel(source, x, y, {2, -1, .25, .5}));
	auto d = Graph();
	d.Nodes[1].Values = {{"interpolate", EnumValue{6}}};
	auto plan = Compiled(d);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output;
	Diagnostic diag;
	REQUIRE(Evaluate(d, plan, "mapped", request, output, diag) == Status::Ok);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(output, 2, 3, pixel));
	CHECK(pixel == SurfacePixel{2, -1, .25, .5});
	const auto prior = output;
	CHECK(Evaluate(d, plan, "mapped", request, output, diag, 1) == Status::LimitExceeded);
	CHECK(output == prior);
	d.Nodes[1].Values.push_back({"spokes", 0.});
	plan = Compiled(d);
	CHECK(Evaluate(d, plan, "mapped", request, output, diag) == Status::InvalidValue);
	CHECK(diag.Port == "spokes");
	CHECK(output == prior);
	CHECK(sources[0].Data == source);
}
TEST_CASE(
	"Polar Mirror CleanEdge quotes worst original interpolation before pixel allocation",
	"[source_2d][mirror_polar]"
) {
	const auto source = Coordinates();
	Node node{"polar", "pc.mirror_polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = 1;
	context.ProcessorCount = 256;
	context.Images = {{"surface_in", &source}};
	context.Values = {{"interpolate", EnumValue{1}}};
	const Value modes = ArrayValue{ValueType::Enum, {EnumValue{1}, EnumValue{6}}};
	const std::array<std::pair<std::string_view, const Value *>, 1> original{{{"interpolate", &modes}}};
	context.ProcessorOriginalValues = original;
	CHECK_FALSE(detail::FindExecutor(node.Type)(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(context.OutputImages.empty());
	detail::NodeContext inherited(node, *FindCatalogueEntry(node.Type), request);
	inherited.ByteBudget = 1;
	inherited.ProcessorCount = 256;
	inherited.Images = context.Images;
	inherited.InheritedInterpolation = 6;
	CHECK_FALSE(detail::FindExecutor(node.Type)(inherited));
	CHECK(inherited.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(inherited.OutputImages.empty());
}

TEST_CASE(
	"Polar Mirror CleanEdge diagonal source slice fills a corner that nearest leaves black",
	"[source_2d][mirror_polar]"
) {
	Image source{5, 5, std::vector<uint8_t>(5 * 5 * 4), 0};
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 5; ++x) {
			const uint8_t c = x + y >= 5 ? 255 : 0;
			const size_t i = (y * 5 + x) * 4;
			source.Pixels[i] = source.Pixels[i + 1] = source.Pixels[i + 2] = c;
			source.Pixels[i + 3] = 255;
		}
	auto render = [&](int64_t mode) {
		return RunNode(
			"pc.mirror_polar",
			{{"surface_in", &source}},
			{{"interpolate", EnumValue{mode}},
			 {"spokes", 1.},
			 {"center", Vector2{2.01, 7.99 / 3.}},
			 {"center_unit", EnumValue{0}},
			 {"scale", Vector2{1, 2}}}
		);
	};
	const auto nearest = render(1), clean = render(6);
	INFO(clean.Message);
	REQUIRE(nearest.Ok);
	REQUIRE(clean.Ok);
	// Source polar equations give uv=(2.99/5,2.99/5), local=(.9900598,.9900598).
	// Up and Back reject. Corner has F=D=white,C=black; signed line distance(.02/sqrt2)<.5.
	Pixel(nearest.Output(), 2, 2, {0, 0, 0, 255});
	Pixel(clean.Output(), 2, 2, {255, 255, 255, 255});
}
