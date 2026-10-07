#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_polar")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Labels2x2() {
		return {2, 2, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255}, 0};
	}
	Image XLabels4x4() {
		Image image{4, 4, std::vector<uint8_t>(64), 0};
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x)
				image.Pixels[(size_t(y) * image.Width + x) * 4] = uint8_t((x + 1) * 10);
		for (size_t pixel = 0; pixel < 16; ++pixel)
			image.Pixels[pixel * 4 + 3] = 255;
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x, uint32_t y) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	Image FloatMap(SurfacePixel colour) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, colour));
		return image;
	}
	Document PolarGraph(Value angle = 0.) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"polar", "pc.polar", "", {}, {{"angle", std::move(angle)}, {"interpolate", EnumValue{1}}}}
		};
		document.Links = {{"source", "image", "polar", "surface_in"}};
		document.Outputs = {{"out", "polar", "surface_out"}};
		return document;
	}
	AtlasValue SurfaceAtlas(Image source) {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = std::move(source);
		data.Dimension = {1, 1};
		return atlas;
	}
}

TEST_CASE("Polar maps 2x2 pixels through source angle and tile equations", "[source_2d][polar]") {
	const auto source = Labels2x2();
	const auto run = RunNode("pc.polar", {{"surface_in", &source}}, {{"interpolate", EnumValue{1}}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(
		run.Output().Pixels ==
		std::vector<uint8_t>{20, 0, 0, 255, 20, 0, 0, 255, 40, 0, 0, 255, 40, 0, 0, 255}
	);

	const auto identity = RunNode("pc.polar", {{"surface_in", &source}}, {{"blend", 0.}});
	REQUIRE(identity.Ok);
	CHECK(identity.Output().Pixels == source.Pixels);
	const auto inverse =
		RunNode("pc.polar", {{"surface_in", &source}}, {{"invert", true}, {"center", Vector2{.4, .5}}});
	REQUIRE(inverse.Ok);
	CHECK(
		inverse.Output().Pixels ==
		std::vector<uint8_t>{30, 0, 0, 255, 30, 0, 0, 255, 10, 0, 0, 255, 10, 0, 0, 255}
	);
	const auto swapped = RunNode("pc.polar", {{"surface_in", &source}}, {{"swap_axis", true}});
	REQUIRE(swapped.Ok);
	CHECK(
		swapped.Output().Pixels ==
		std::vector<uint8_t>{30, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255, 40, 0, 0, 255}
	);
}

TEST_CASE("Polar supports all source radius and interpolation modes", "[source_2d][polar]") {
	const auto source = XLabels4x4();
	const std::array<int64_t, 3> radii{0, 1, 2};
	const std::array<uint8_t, 3> expected{20, 30, 30};
	for (size_t index = 0; index < radii.size(); ++index) {
		const auto run = RunNode(
			"pc.polar",
			{{"surface_in", &source}},
			{{"radius_mode", EnumValue{radii[index]}}, {"interpolate", EnumValue{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[(size_t(1) * 4 + 1) * 4] == expected[index]);
	}
	for (int64_t interpolation : {1, 2, 3, 4, 6}) {
		const auto run =
			RunNode("pc.polar", {{"surface_in", &source}}, {{"interpolate", EnumValue{interpolation}}});
		INFO("interpolation " << interpolation << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Width == source.Width);
		CHECK(run.Output().Height == source.Height);
		for (size_t offset = 0; offset < run.Output().Pixels.size(); offset += 4)
			CHECK(run.Output().Pixels[offset + 3] == 255);
	}
	const auto originLog = RunNode(
		"pc.polar",
		{{"surface_in", &source}},
		{{"radius_mode", EnumValue{2}}, {"center", Vector2{1.5, 1.5}}, {"center_unit", EnumValue{0}}}
	);
	CHECK_FALSE(originLog.Ok);
	CHECK(originLog.Code == Status::InvalidValue);
	CHECK(originLog.Port == "radius_mode");
}

TEST_CASE("Polar mapped angle blend and twist use the linked map ranges", "[source_2d][polar]") {
	const auto source = Labels2x2();
	const auto black = FloatMap({0, 0, 0, 1});
	const auto white = FloatMap({1, 1, 1, 1});
	const auto scalarAngle = RunNode("pc.polar", {{"surface_in", &source}}, {{"angle", 90.}});
	const auto mappedAngle = RunNode(
		"pc.polar",
		{{"surface_in", &source}, {"angle_map", &white}},
		{{"angle_mapped", true}, {"angle_map_range", Vector2{0, 90}}}
	);
	REQUIRE(scalarAngle.Ok);
	REQUIRE(mappedAngle.Ok);
	CHECK(mappedAngle.Output().Pixels == scalarAngle.Output().Pixels);
	const auto missingAngleMap = RunNode(
		"pc.polar", {{"surface_in", &source}}, {{"angle_mapped", true}, {"angle_map_range", Vector2{0, 90}}}
	);
	const auto lowAngle = RunNode("pc.polar", {{"surface_in", &source}}, {{"angle", 0.}});
	REQUIRE(missingAngleMap.Ok);
	REQUIRE(lowAngle.Ok);
	CHECK(missingAngleMap.Output().Pixels == lowAngle.Output().Pixels);

	const auto lowBlend = RunNode("pc.polar", {{"surface_in", &source}}, {{"blend", 0.}});
	const auto mappedBlend = RunNode(
		"pc.polar",
		{{"surface_in", &source}, {"blend_map", &black}},
		{{"blend_mapped", true}, {"blend_map_range", Vector2{0, 1}}}
	);
	REQUIRE(lowBlend.Ok);
	REQUIRE(mappedBlend.Ok);
	CHECK(mappedBlend.Output().Pixels == lowBlend.Output().Pixels);

	const auto scalarTwist = RunNode("pc.polar", {{"surface_in", &source}}, {{"twist", 180.}});
	const auto mappedTwist = RunNode(
		"pc.polar",
		{{"surface_in", &source}, {"twist_map", &white}},
		{{"twist_mapped", true}, {"twist_map_range", Vector2{0, 180}}}
	);
	REQUIRE(scalarTwist.Ok);
	REQUIRE(mappedTwist.Ok);
	CHECK(mappedTwist.Output().Pixels == scalarTwist.Output().Pixels);

	const auto unusedMap = RunNode("pc.polar", {{"surface_in", &source}, {"angle_map", &black}});
	const auto defaultAngle = RunNode("pc.polar", {{"surface_in", &source}});
	REQUIRE(unusedMap.Ok);
	REQUIRE(defaultAngle.Ok);
	CHECK(unusedMap.Output().Pixels == defaultAngle.Output().Pixels);

	auto mappedGraph = PolarGraph(90.);
	mappedGraph.Nodes[1].Values.push_back({"angle_mapped", true});
	mappedGraph.Nodes[1].Values.push_back({"angle_map_range", Vector2{0, 90}});
	mappedGraph.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string("angle-map")}}});
	mappedGraph.Links.push_back({"map", "image", "polar", "angle_map"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(mappedGraph, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 2> graphSources{{{"source", source}, {"angle-map", white}}};
	EvaluationRequest request;
	request.ImageSources = graphSources;
	Image graphOutput;
	REQUIRE(Evaluate(mappedGraph, plan, "out", request, graphOutput, diagnostic) == Status::Ok);
	CHECK(graphOutput.Pixels == scalarAngle.Output().Pixels);
}

TEST_CASE("Polar center units and range validation preserve authored coordinates", "[source_2d][polar]") {
	const auto source = XLabels4x4();
	const auto pixels = RunNode(
		"pc.polar",
		{{"surface_in", &source}},
		{{"center", Vector2{2, 2}}, {"center_unit", EnumValue{0}}, {"interpolate", EnumValue{1}}}
	);
	const auto normalized = RunNode(
		"pc.polar",
		{{"surface_in", &source}},
		{{"center", Vector2{.5, .5}}, {"center_unit", EnumValue{1}}, {"interpolate", EnumValue{1}}}
	);
	INFO(pixels.Message << " / " << normalized.Message);
	REQUIRE(pixels.Ok);
	REQUIRE(normalized.Ok);
	CHECK(pixels.Output().Pixels == normalized.Output().Pixels);

	const auto invalid = RunNode("pc.polar", {{"surface_in", &source}}, {{"range", Vector2{30, 30}}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
	CHECK(invalid.Port == "range");
	const auto clipped = RunNode(
		"pc.polar", {{"surface_in", &source}}, {{"range", Vector2{90, 270}}, {"interpolate", EnumValue{1}}}
	);
	REQUIRE(clipped.Ok);
	CHECK(Pixel(clipped.Output(), 1, 0)[3] == 0);
}

TEST_CASE("Polar mask mix and channel selection use the shared processor finish", "[source_2d][polar]") {
	const auto source = Labels2x2();
	const auto warped = RunNode("pc.polar", {{"surface_in", &source}});
	const auto zeroMix = RunNode("pc.polar", {{"surface_in", &source}}, {{"mix", 0.}});
	const auto blackMaskImage = FloatMap({0, 0, 0, 1});
	const auto blackMask = RunNode("pc.polar", {{"surface_in", &source}, {"mask", &blackMaskImage}});
	REQUIRE(warped.Ok);
	REQUIRE(zeroMix.Ok);
	REQUIRE(blackMask.Ok);
	CHECK(zeroMix.Output().Pixels == source.Pixels);
	CHECK(blackMask.Output().Pixels == source.Pixels);

	const Image mask{1, 1, {255, 255, 255, 255}, 0};
	const auto selected = RunNode(
		"pc.polar", {{"surface_in", &source}, {"mask", &mask}}, {{"mix", .5}, {"channel", int64_t{1}}}
	);
	REQUIRE(selected.Ok);
	for (size_t pixel = 0; pixel < 4; ++pixel) {
		const size_t offset = pixel * 4;
		CHECK(
			selected.Output().Pixels[offset] ==
			uint8_t((source.Pixels[offset] + warped.Output().Pixels[offset]) / 2)
		);
		CHECK(selected.Output().Pixels[offset + 1] == source.Pixels[offset + 1]);
		CHECK(selected.Output().Pixels[offset + 2] == source.Pixels[offset + 2]);
		CHECK(selected.Output().Pixels[offset + 3] == source.Pixels[offset + 3]);
	}
}

TEST_CASE("Polar inactive copies typed source and accepts only the main SurfaceAtlas", "[source_2d][polar]") {
	Image hdr{2, 2, std::vector<uint8_t>(64), 0, SurfaceFormat::RGBA32Float};
	for (uint32_t y = 0; y < 2; ++y)
		for (uint32_t x = 0; x < 2; ++x)
			REQUIRE(StoreSurfacePixel(hdr, x, y, {2. + x, .5, .25, 1}));
	const auto inactive =
		RunNode("pc.polar", {{"surface_in", &hdr}}, {{"active", false}, {"radius_mode", EnumValue{99}}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(inactive.Output().Pixels == hdr.Pixels);

	for (SurfaceFormat format :
		 {SurfaceFormat::RGBA8Unorm, SurfaceFormat::RGBA16Float, SurfaceFormat::RGBA32Float}) {
		const auto layout = CheckedSurfaceLayout(2, 2, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image source{2, 2, std::vector<uint8_t>(size_t(layout->Bytes)), 0, format};
		REQUIRE(StoreSurfacePixel(source, 0, 0, {.25, .5, .75, 1}));
		auto run = RunNode("pc.polar", {{"surface_in", &source}}, {{"blend", 0.}});
		INFO("format " << unsigned(format) << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == format);
		CHECK(Pixel(run.Output(), 0, 0) == Pixel(source, 0, 0));
	}
	const auto hdrRun = RunNode("pc.polar", {{"surface_in", &hdr}}, {{"blend", 1.}});
	REQUIRE(hdrRun.Ok);
	CHECK(hdrRun.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(Pixel(hdrRun.Output(), 0, 0)[0] > 1);

	const auto source = Labels2x2();
	const auto main = RunNode("pc.polar", {}, {{"surface_in", SurfaceAtlas(source)}});
	const auto plain = RunNode("pc.polar", {{"surface_in", &source}});
	REQUIRE(main.Ok);
	REQUIRE(plain.Ok);
	CHECK(main.Output().Pixels == plain.Output().Pixels);
	const auto raw = RunNode(
		"pc.polar",
		{{"surface_in", &source}},
		{{"angle_map", SurfaceAtlas(source)}, {"angle_mapped", true}, {"angle_map_range", Vector2{0, 90}}}
	);
	CHECK_FALSE(raw.Ok);
	CHECK(raw.Code == Status::UnsupportedExecution);
	CHECK(raw.Port == "angle_map");
}

TEST_CASE("Polar graph arrays preserve enum and scalar row values", "[source_2d][polar]") {
	auto source = XLabels4x4();
	// grug angle changes sampled Y, so fixture must distinguish rows too.
	for (uint32_t y = 0; y < source.Height; ++y)
		for (uint32_t x = 0; x < source.Width; ++x)
			source.Pixels[(size_t(y) * source.Width + x) * 4] += uint8_t(y * 40);
	auto document = PolarGraph();
	ArrayValue radii{ValueType::Enum, {EnumValue{0}, EnumValue{1}}};
	ArrayValue angles{ValueType::Scalar, {0., 90.}};
	document.Junctions = {
		{"radii", "", ValueType::Array, std::move(radii)}, {"angles", "", ValueType::Array, std::move(angles)}
	};
	document.Links.push_back({"radii", "value", "polar", "radius_mode"});
	document.Links.push_back({"angles", "value", "polar", "angle"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(document, plan, "out", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Pixels != output.Images[1].Pixels);

	auto animated = PolarGraph(0.);
	animated.Nodes[1].SourceAnimatedInputs = {"angle"};
	animated.Keyframes = {{"polar", "angle", 0, 0.}, {"polar", "angle", 1, 90.}};
	Document restored;
	REQUIRE(Read(Write(animated), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == animated);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	request.Tick = 0;
	Image first;
	REQUIRE(Evaluate(restored, plan, "out", request, first, diagnostic) == Status::Ok);
	request.Tick = 1;
	Image later;
	REQUIRE(Evaluate(restored, plan, "out", request, later, diagnostic) == Status::Ok);
	CHECK(first.Pixels != later.Pixels);
	const auto directFirst = RunNode("pc.polar", {{"surface_in", &source}}, {{"angle", 0.}});
	const auto directLater = RunNode("pc.polar", {{"surface_in", &source}}, {{"angle", 90.}});
	REQUIRE(directFirst.Ok);
	REQUIRE(directLater.Ok);
	CHECK(first.Pixels == directFirst.Output().Pixels);
	CHECK(later.Pixels == directLater.Output().Pixels);
	const auto scalarLinked = PolarGraph(0.);
	auto linked = scalarLinked;
	linked.Junctions = {{"angle", "", ValueType::Scalar, 90.}};
	linked.Links.push_back({"angle", "value", "polar", "angle"});
	REQUIRE(Compile(linked, plan, diagnostic) == Status::Ok);
	Image linkedOutput;
	REQUIRE(Evaluate(linked, plan, "out", request, linkedOutput, diagnostic) == Status::Ok);
	CHECK(linkedOutput.Pixels == later.Pixels);
}

TEST_CASE("Polar cumulative native work is admitted before any row observer fires", "[source_2d][polar]") {
	Image source{120, 120, std::vector<uint8_t>(120 * 120 * 4, 255), 0};
	const auto *entry = FindCatalogueEntry("pc.polar");
	REQUIRE(entry);
	Node node{"polar", "pc.polar", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	for (const auto &input : entry->Inputs)
		if (auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, std::move(*value));
	bool hasInterpolation = false, hasAngle = false;
	for (auto &[port, value] : context.Values) {
		if (port == "interpolate") {
			value = EnumValue{6};
			hasInterpolation = true;
		}
		if (port == "angle") {
			value = ArrayValue{ValueType::Scalar, {0., 90.}};
			hasAngle = true;
		}
	}
	if (!hasInterpolation) context.Values.emplace_back("interpolate", EnumValue{6});
	if (!hasAngle) context.Values.emplace_back("angle", ArrayValue{ValueType::Scalar, {0., 90.}});
	context.InputProvenanceResolved = true;
	size_t observed = 0;
	const auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	const auto execute = detail::FindExecutor("pc.polar");
	REQUIRE(execute);
	CHECK_FALSE(detail::RunProcessorBatch(context, execute, observer, &observed));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("complete batch") != std::string::npos);
	CHECK(observed == 0);
	CHECK(context.OutputImages.empty());
}

TEST_CASE(
	"Polar reference center converts host doubles before uploading float uniforms", "[source_2d][polar]"
) {
	Image source{7, 5, std::vector<uint8_t>(7 * 5 * 16), 0, SurfaceFormat::RGBA32Float};
	for (uint32_t y = 0; y < source.Height; ++y)
		for (uint32_t x = 0; x < source.Width; ++x)
			REQUIRE(StoreSurfacePixel(source, x, y, {double(x) / 7, double(y) / 5, .25, 1}));
	const Vector2 reference{.1, .3}, pixelCenter{reference.X * source.Width, reference.Y * source.Height};
	const auto relative = RunNode(
		"pc.polar",
		{{"surface_in", &source}},
		{{"center", reference}, {"center_unit", EnumValue{1}}, {"interpolate", EnumValue{2}}}
	);
	const auto pixels = RunNode(
		"pc.polar",
		{{"surface_in", &source}},
		{{"center", pixelCenter}, {"center_unit", EnumValue{0}}, {"interpolate", EnumValue{2}}}
	);
	REQUIRE(relative.Ok);
	REQUIRE(pixels.Ok);
	CHECK(relative.Output().Pixels == pixels.Output().Pixels);
	auto document = PolarGraph();
	for (auto &value : document.Nodes[1].Values)
		if (value.Port == "interpolate") value.Data = EnumValue{2};
	document.Junctions = {{"center", "", ValueType::Vector2, pixelCenter}};
	document.Links.push_back({"center", "value", "polar", "center"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image linked;
	const auto status = Evaluate(document, plan, "out", request, linked, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(linked.Pixels == pixels.Output().Pixels);
}

TEST_CASE("Polar map texture stage remains nearest when the main texture is bilinear", "[source_2d][polar]") {
	const auto source = XLabels4x4();
	const Image map{2, 2, {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255}, 0};
	const auto run = RunNode(
		"pc.polar",
		{{"surface_in", &source}, {"blend_map", &map}},
		{{"blend_mapped", true}, {"blend_map_range", Vector2{0, 1}}, {"interpolate", EnumValue{2}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output(), 1, 1) == Pixel(source, 1, 1));
}

TEST_CASE("Polar Expand arrays route source slots and Oversample remains unused", "[source_2d][polar]") {
	const auto source = Labels2x2();
	auto document = PolarGraph(ArrayValue{ValueType::Scalar, {0., 90.}});
	document.Nodes[1].Values.push_back({"blend", ArrayValue{ValueType::Scalar, {0., .5, 1.}}});
	document.Nodes[1].Values.push_back({"attribute_array_process", EnumValue{2}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	const auto status = EvaluateArray(document, plan, "out", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 6);
	const auto empty =
		RunNode("pc.polar", {{"surface_in", &source}}, {{"blend", 5.}, {"oversample", EnumValue{1}}});
	const auto repeat =
		RunNode("pc.polar", {{"surface_in", &source}}, {{"blend", 5.}, {"oversample", EnumValue{4}}});
	REQUIRE(empty.Ok);
	REQUIRE(repeat.Ok);
	CHECK(empty.Output().Pixels == repeat.Output().Pixels);
	const auto huge =
		RunNode("pc.polar", {{"surface_in", &source}}, {{"angle", std::numeric_limits<double>::max()}});
	CHECK_FALSE(huge.Ok);
	CHECK(huge.Code == Status::InvalidValue);
	CHECK(huge.Port == "angle");
}
