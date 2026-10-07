#include "NodeHarness.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_area_warp")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Coordinates(uint32_t width = 4, uint32_t height = 4) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const size_t p = (size_t(y) * width + x) * 4;
				image.Pixels[p] = uint8_t(x * 40);
				image.Pixels[p + 1] = uint8_t(y * 40);
				image.Pixels[p + 3] = 255;
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
			{"warp", "pc.wrap_area", "", {}, {{"area_unit", EnumValue{1}}}}
		};
		d.Links = {{"source", "image", "warp", "surface_in"}};
		d.Outputs = {{"out", "warp", "surface_out"}};
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
TEST_CASE("Area Warp defaults to an identity image", "[source_2d][area_warp]") {
	auto source = Coordinates();
	using engine::core::FrameGraph;
	const bool enabled = FrameGraph::IsEnabled();
	struct Restore {
		bool Enabled;
		~Restore() {
			FrameGraph::SetEnabled(Enabled);
		}
	} restore{enabled};
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	auto result = RunNode("pc.wrap_area", {{"surface_in", &source}});
	FrameGraph::EndFrame();
	CHECK(FrameGraph::Dropped() == 0);
	const auto spans = FrameGraph::Spans();
	CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
		return span.Name == "imagegraph.source.area_warp";
	}));
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(result.Output().Width == source.Width);
	CHECK(result.Output().Height == source.Height);
	CHECK(result.Output().Pixels == source.Pixels);
}
TEST_CASE("Area Warp draws the selected pixel rectangle and clips outside", "[source_2d][area_warp]") {
	auto source = Coordinates();
	auto result = RunNode(
		"pc.wrap_area", {{"surface_in", &source}}, {{"area", Area{2, 2, 1, 1}}, {"area_unit", EnumValue{0}}}
	);
	REQUIRE(result.Ok);
	for (uint32_t y = 0; y < 4; ++y)
		for (uint32_t x = 0; x < 4; ++x) {
			const size_t p = (size_t(y) * 4 + x) * 4;
			if (x >= 1 && x < 3 && y >= 1 && y < 3)
				CHECK(result.Output().Pixels[p + 3] == 255);
			else
				CHECK(result.Output().Pixels[p + 3] == 0);
		}
	Pixel(result.Output(), 1, 1, {40, 40, 0, 255});
	Pixel(result.Output(), 2, 2, {120, 120, 0, 255});
}
TEST_CASE("Area Warp negative horizontal halfwidth mirrors the selected area", "[source_2d][area_warp]") {
	auto source = Coordinates();
	auto mirrored = RunNode(
		"pc.wrap_area",
		{{"surface_in", &source}},
		{{"area", Area{2, 2, -1.5, 1}}, {"area_unit", EnumValue{0}}}
	);
	REQUIRE(mirrored.Ok);
	Pixel(mirrored.Output(), 0, 1, {120, 40, 0, 255});
	Pixel(mirrored.Output(), 1, 1, {80, 40, 0, 255});
	Pixel(mirrored.Output(), 2, 1, {40, 40, 0, 255});
	CHECK(mirrored.Output().Pixels[(size_t(1) * 4 + 3) * 4 + 3] == 0);
}
TEST_CASE(
	"Area Warp normalizes authored padding and two point areas before reference units",
	"[source_2d][area_warp]"
) {
	auto source = Coordinates();
	for (Area area : {Area{.25, .25, .25, .25, 0, 1}, Area{.25, .25, .75, .75, 0, 2}}) {
		auto result =
			RunNode("pc.wrap_area", {{"surface_in", &source}}, {{"area", area}, {"area_unit", EnumValue{1}}});
		INFO("area mode " << unsigned(area.Mode));
		INFO(result.Message);
		REQUIRE(result.Ok);
		CHECK(result.Output().Pixels[(size_t(1) * 4 + 1) * 4 + 3] == 255);
		CHECK(result.Output().Pixels[(size_t(0) * 4 + 0) * 4 + 3] == 0);
	}
}
TEST_CASE(
	"Area Warp zero area is transparent and inactive copies before rectangle checks", "[source_2d][area_warp]"
) {
	auto source = Coordinates();
	auto zero = RunNode(
		"pc.wrap_area", {{"surface_in", &source}}, {{"area", Area{2, 2, 0, 1}}, {"area_unit", EnumValue{0}}}
	);
	REQUIRE(zero.Ok);
	for (size_t p = 3; p < zero.Output().Pixels.size(); p += 4)
		CHECK(zero.Output().Pixels[p] == 0);
	auto inactive = RunNode(
		"pc.wrap_area",
		{{"surface_in", &source}},
		{{"active", false}, {"area", Area{0, 0, 0, 0, 0, 9}}, {"area_unit", EnumValue{-1}}}
	);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == source.Pixels);
}
TEST_CASE("Area Warp preserves float samples and safely expands red surfaces", "[source_2d][area_warp]") {
	Image floating{2, 1, std::vector<uint8_t>(2 * 16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(floating, 0, 0, {-2, 3, .25, .5}));
	REQUIRE(StoreSurfacePixel(floating, 1, 0, {4, -1, .75, 1}));
	auto retained = RunNode("pc.wrap_area", {{"surface_in", &floating}});
	INFO(retained.Message);
	REQUIRE(retained.Ok);
	CHECK(retained.Output().Format == SurfaceFormat::RGBA32Float);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(retained.Output(), 0, 0, pixel));
	CHECK((pixel == SurfacePixel{-2, 3, .25, .5}));
	Image red{2, 1, {32, 192}, 0, SurfaceFormat::R8Unorm};
	auto expanded =
		RunNode("pc.wrap_area", {{"surface_in", &red}}, {{"attribute_color_depth", EnumValue{3}}});
	REQUIRE(expanded.Ok);
	CHECK(expanded.Output().Format == SurfaceFormat::RGBA8Unorm);
	Pixel(expanded.Output(), 0, 0, {32, 32, 32, 255});
	Pixel(expanded.Output(), 1, 0, {192, 192, 192, 255});
}
TEST_CASE(
	"Area Warp compiled graph survives save and evaluates a captured source", "[source_2d][area_warp]"
) {
	auto document = Graph();
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	auto plan = Compiled(restored);
	const std::array<RequestImageSource, 1> sources{{{"source", Coordinates()}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output;
	REQUIRE(Evaluate(restored, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == sources.front().Data.Pixels);
	CHECK(output.Width == sources.front().Data.Width);
	CHECK(output.Format == sources.front().Data.Format);
}

TEST_CASE(
	"Area Warp admits inherited sampler work for the entire original batch before allocation",
	"[source_2d][area_warp]"
) {
	const auto *entry = FindCatalogueEntry("pc.wrap_area");
	REQUIRE(entry);
	Node node{"warp", "pc.wrap_area", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	auto source = Coordinates(128, 128);
	context.Images.emplace_back("surface_in", &source);
	context.Values.emplace_back("interpolate", EnumValue{1});
	context.Values.emplace_back("area", Area{});
	ArrayValue modes;
	modes.ElementType = ValueType::Enum;
	modes.Elements = {EnumValue{1}, EnumValue{0}};
	Value original = modes;
	const std::array<std::pair<std::string_view, const Value *>, 1> originals{{{"interpolate", &original}}};
	context.ProcessorOriginalValues = originals;
	context.ProcessorCount = 2;
	context.InheritedInterpolation = 6;
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	CHECK_FALSE(detail::FindExecutor("pc.wrap_area")(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_in");
	CHECK(context.OutputImages.empty());
}
TEST_CASE(
	"Area Warp reports malformed and nonfinite rectangle controls without publishing",
	"[source_2d][area_warp]"
) {
	auto source = Coordinates();
	for (Value area :
		 {Value{double{1}},
		  Value{Area{std::numeric_limits<double>::max(), 2, std::numeric_limits<double>::max(), 1}}}) {
		auto result = RunNode("pc.wrap_area", {{"surface_in", &source}}, {{"area", area}});
		CHECK_FALSE(result.Ok);
		CHECK(result.Port == "area");
		CHECK(result.Images.empty());
	}
}

TEST_CASE(
	"Area Warp preserves grouped image arrays with linked rectangles and inherited scalar sampling",
	"[source_2d][area_warp]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Groups = {{"group", "warp group"}};
	d.Groups[0].Interpolation = 6;
	d.Nodes = {
		{"solid",
		 "pc.solid",
		 "group",
		 {},
		 {{"dimension_unit", EnumValue{0}}, {"color", Colour{64, 128, 192, 128}}}},
		{"warp", "pc.wrap_area", "group", {}, {}}
	};
	d.Junctions = {
		{"sizes", "group", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{3, 3}, Vector2{5, 5}}}},
		{"areas", "group", ValueType::Array, ArrayValue{ValueType::Area, {Area{}, Area{.5, .5, .3, .3}}}},
		{"samplers", "group", ValueType::Enum, EnumValue{0}},
		{"sampler_array",
		 "group",
		 ValueType::Array,
		 ArrayValue{ValueType::Enum, {EnumValue{1}, EnumValue{0}}}}
	};
	d.Links = {
		{"sizes", "value", "solid", "dimension"},
		{"solid", "surface_out", "warp", "surface_in"},
		{"areas", "value", "warp", "area"},
		{"samplers", "value", "warp", "interpolate"}
	};
	d.Outputs = {{"out", "warp", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	const auto read = Read(Write(d), restored, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(read == Status::Ok);
	CHECK(restored == d);
	auto plan = Compiled(restored);
	ImageArray output;
	const auto evaluated = EvaluateArray(restored, plan, "out", {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Width == 3);
	CHECK(output.Images[1].Width == 5);
	Pixel(output.Images[0], 0, 0, {64, 128, 192, 128});
	Pixel(output.Images[1], 0, 0, {0, 0, 0, 0});
	Pixel(output.Images[1], 2, 2, {64, 128, 192, 128});
	const auto prior = output;
	restored.Links.back().FromNode = "sampler_array";
	plan = Compiled(restored);
	const auto refused = EvaluateArray(restored, plan, "out", {}, output, diagnostic);
	INFO(diagnostic.Message);
	CHECK(refused == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "interpolate");
	CHECK(output.Images == prior.Images);
	CHECK(output.Items == prior.Items);
}
TEST_CASE(
	"Area Warp refuses an expensive later image before publishing any batch output", "[source_2d][area_warp]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Nodes = {
		{"solid", "pc.solid", "", {}, {{"dimension_unit", EnumValue{0}}}},
		{"warp", "pc.wrap_area", "", {}, {}}
	};
	d.Junctions = {
		{"sizes", "", ValueType::Array, ArrayValue{ValueType::Vector2, {Vector2{2, 2}, Vector2{1024, 1024}}}}
	};
	d.Links = {{"sizes", "value", "solid", "dimension"}, {"solid", "surface_out", "warp", "surface_in"}};
	d.Outputs = {{"out", "warp", "surface_out"}};
	auto plan = Compiled(d);
	ImageArray output;
	output.Images = {{1, 1, {9, 8, 7, 6}, 0}};
	const auto prior = output;
	Diagnostic diagnostic;
	CHECK(EvaluateArray(d, plan, "out", {}, output, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "warp");
	CHECK(diagnostic.Port == "surface_in");
	CHECK(output.Images == prior.Images);
	CHECK(output.Items == prior.Items);
}

TEST_CASE(
	"Area Warp ordinary and Atlas paths retain every numeric surface format", "[source_2d][area_warp]"
) {
	for (SurfaceFormat format :
		 {SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		const auto layout = CheckedSurfaceLayout(2, 2, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image image{2, 2, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 2; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, {.25, .5, .75, .5}));
		AtlasValue atlas;
		atlas.Data.emplace().Surface.Data = image;
		atlas.Data->Kind = AtlasKind::SurfaceAtlas;
		for (bool active : {false, true})
			for (bool wrapped : {false, true}) {
				auto result = wrapped
								  ? RunNode("pc.wrap_area", {}, {{"surface_in", atlas}, {"active", active}})
								  : RunNode("pc.wrap_area", {{"surface_in", &image}}, {{"active", active}});
				INFO(result.Message);
				REQUIRE(result.Ok);
				CHECK(result.Output().Format == format);
				CHECK(result.Output().Pixels == image.Pixels);
			}
	}
}

TEST_CASE("Area Warp rejects base Atlas payloads as nonsurface inputs", "[source_2d][area_warp]") {
	AtlasValue atlas;
	atlas.Data.emplace().Surface.Data = Coordinates();
	atlas.Data->Kind = AtlasKind::Atlas;
	atlas.Data->Position = {1, 2};
	for (bool active : {false, true}) {
		const auto result = RunNode("pc.wrap_area", {}, {{"surface_in", atlas}, {"active", active}});
		CHECK_FALSE(result.Ok);
		CHECK(result.Code == Status::UnsupportedExecution);
		CHECK(result.Port == "surface_in");
		CHECK(result.Images.empty());
	}
}
TEST_CASE(
	"Area Warp typed control arrays retain nested payloads and reject mismatched leaves",
	"[source_2d][area_warp]"
) {
	auto document = Graph();
	ArrayValue areas;
	areas.ElementType = ValueType::Area;
	areas.Nested = {{Area{}, Area{.5, .5, .3, .3}}};
	ArrayValue samplers;
	samplers.ElementType = ValueType::Enum;
	samplers.Nested = {{EnumValue{1}, EnumValue{0}}};
	document.Junctions = {
		{"areas", "", ValueType::Array, areas}, {"samplers", "", ValueType::Array, samplers}
	};
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Compiled(restored);
	document.Junctions[0].Default = ArrayValue{ValueType::Area, {EnumValue{1}}};
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	document.Junctions[0].Default =
		ArrayValue{ValueType::Area, {Area{std::numeric_limits<double>::infinity(), 0, 1, 1}}};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Area Warp refuses malformed SurfaceAtlas before allocating a draw target", "[source_2d][area_warp]"
) {
	for (int fault : {0, 1, 2}) {
		AtlasValue atlas;
		if (fault != 0) {
			atlas.Data.emplace().Surface.Data = Coordinates();
			atlas.Data->Kind = AtlasKind::SurfaceAtlas;
			if (fault == 1)
				atlas.Data->Position.X = std::numeric_limits<double>::infinity();
			else
				atlas.Data->Surface.Data.Pixels.pop_back();
		}
		const auto result = RunNode("pc.wrap_area", {}, {{"surface_in", atlas}});
		CHECK_FALSE(result.Ok);
		CHECK(result.Code == Status::InvalidValue);
		CHECK(result.Port == "surface_in");
		CHECK(result.Images.empty());
	}
}
TEST_CASE(
	"Area Warp admits every SurfaceAtlas image before starting an expensive batch", "[source_2d][area_warp]"
) {
	Document document;
	document.FormatVersion = 9;
	AtlasValue small, large;
	small.Data.emplace().Surface.Data = Coordinates(2, 2);
	large.Data.emplace().Surface.Data = Coordinates(512, 512);
	small.Data->Kind = large.Data->Kind = AtlasKind::SurfaceAtlas;
	document.Nodes = {{"warp", "pc.wrap_area", "", {}, {{"interpolate", EnumValue{6}}}}};
	document.Junctions = {{"atlases", "", ValueType::Array, ArrayValue{ValueType::Atlas, {small, large}}}};
	document.Links = {{"atlases", "value", "warp", "surface_in"}};
	document.Outputs = {{"out", "warp", "surface_out"}};
	auto plan = Compiled(document);
	Diagnostic diagnostic;
	ImageArray result;
	result.Images = {{1, 1, {9, 8, 7, 6}, 0}};
	const auto prior = result;
	const auto evaluated = EvaluateArray(document, plan, "out", {}, result, diagnostic);
	INFO(diagnostic.Message);
	CHECK(evaluated == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "warp");
	CHECK(diagnostic.Port == "surface_in");
	CHECK(result.Images == prior.Images);
	CHECK(result.Items == prior.Items);
}
