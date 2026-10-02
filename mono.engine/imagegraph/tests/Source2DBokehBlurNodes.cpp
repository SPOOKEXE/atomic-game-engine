#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.imagegraph.source_bokeh_blur")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image SolidImage(uint32_t width, uint32_t height, std::array<uint8_t, 4> colour) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * colour.size());
		for (size_t offset = 0; offset < image.Pixels.size(); offset += colour.size())
			for (size_t channel = 0; channel < colour.size(); ++channel)
				image.Pixels[offset + channel] = colour[channel];
		return image;
	}

	Image Uniform() {
		return SolidImage(2, 2, {64, 128, 192, 128});
	}
	Curve Constant(double value) {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 0, 1};
		curve.Anchors = {{0, 0, 0, value, 0, 0}, {0, 0, 1, value, 0, 0}};
		return curve;
	}
	Image Corners() {
		Image image = SolidImage(3, 3, {0, 0, 0, 255});
		image.Pixels[6 * 4 + 1] = 128;
		image.Pixels[2 * 4 + 2] = 192;
		return image;
	}
}
TEST_CASE("Lens Blur preserves its premultiplied RGB and weighted source alpha", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	for (int64_t iteration : {1, 2, 7}) {
		const auto run =
			RunNode("pc.blur_bokeh", {{"surface_in", &image}}, {{"iteration", iteration}, {"strength", 0.}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[0] == 32);
		CHECK(run.Output().Pixels[1] == 64);
		CHECK(run.Output().Pixels[2] == 96);
		CHECK(run.Output().Pixels[3] == 128);
	}
}
TEST_CASE("Lens Blur uses golden-angle row-vector rotation and signed strength", "[imagegraph][source_2d]") {
	const Image image = Corners();
	const auto forward = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"strength", 100.}, {"oversample", EnumValue{4}}}
	);
	INFO(forward.Message);
	REQUIRE(forward.Ok);
	CHECK(forward.Output().Pixels[16] == 0);
	CHECK(forward.Output().Pixels[17] == 128);
	CHECK(forward.Output().Pixels[18] == 0);
	CHECK(forward.Output().Pixels[19] == 255);
	const auto backward = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"strength", -100.}, {"oversample", EnumValue{4}}}
	);
	INFO(backward.Message);
	REQUIRE(backward.Ok);
	CHECK(backward.Output().Pixels[16] == 0);
	CHECK(backward.Output().Pixels[17] == 0);
	CHECK(backward.Output().Pixels[18] == 192);
}
TEST_CASE("Lens Blur colorize and submitted gradient controls are shader-inert", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const Gradient red{0, {{0, {255, 0, 0, 0}}}};
	const auto reference =
		RunNode("pc.blur_bokeh", {{"surface_in", &image}}, {{"iteration", int64_t{3}}, {"strength", 0.}});
	REQUIRE(reference.Ok);
	for (int64_t mode : {1, 2}) {
		const auto run = RunNode(
			"pc.blur_bokeh",
			{{"surface_in", &image}},
			{{"iteration", int64_t{3}},
			 {"strength", 0.},
			 {"colorize", EnumValue{mode}},
			 {"gradient", red},
			 {"intensity", 1e6},
			 {"scale", -2.},
			 {"shift", .5}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == reference.Output().Pixels);
	}
}
TEST_CASE("Lens Blur strength map averages RGB while ignoring map alpha", "[imagegraph][source_2d]") {
	const Image image = Corners(), white = SolidImage(1, 1, {255, 255, 255, 0}),
				black = SolidImage(1, 1, {0, 0, 0, 255});
	const auto mapped = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}, {"strength_map", &white}},
		{{"iteration", int64_t{1}},
		 {"strength_mapped", true},
		 {"strength_map_range", Vector2{0, 100}},
		 {"oversample", EnumValue{4}}}
	);
	INFO(mapped.Message);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels[17] == 128);
	const auto zero = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}, {"strength_map", &black}},
		{{"iteration", int64_t{1}},
		 {"strength_mapped", true},
		 {"strength_map_range", Vector2{0, 100}},
		 {"oversample", EnumValue{4}}}
	);
	REQUIRE(zero.Ok);
	CHECK(zero.Output().Pixels[17] == 0);
	const auto unbound = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}},
		 {"strength_mapped", true},
		 {"strength_map_range", Vector2{100, 200}},
		 {"oversample", EnumValue{4}}}
	);
	REQUIRE(unbound.Ok);
	CHECK(unbound.Output().Pixels[17] == 128);
}
TEST_CASE("Lens Blur curves multiply weights cubically and diagnose zero totals", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const auto defined = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{3}},
		 {"strength", 0.},
		 {"strength_curved", true},
		 {"strength_curve", Constant(-.5)}}
	);
	INFO(defined.Message);
	REQUIRE(defined.Ok);
	CHECK(defined.Output().Pixels[0] == 32);
	CHECK(defined.Output().Pixels[3] == 128);
	const auto zero = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"strength_curved", true}, {"strength_curve", Constant(0)}}
	);
	CHECK(zero.Code == Status::UnsupportedExecution);
	Curve oversized = Constant(1);
	oversized.Anchors.resize(10, oversized.Anchors.back());
	const auto tooMany = RunNode(
		"pc.blur_bokeh", {{"surface_in", &image}}, {{"strength_curved", true}, {"strength_curve", oversized}}
	);
	CHECK(tooMany.Code == Status::UnsupportedExecution);
	CHECK(tooMany.Port == "strength_curve");
}
TEST_CASE(
	"Lens Blur UV sampling consumes iteration fraction and ignores UV alpha", "[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255}),
				uv = SolidImage(1, 1, {255, 255, 0, 0});
	const auto run = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}, {"uv_map", &uv}},
		{{"iteration", int64_t{2}},
		 {"strength", 0.},
		 {"contrast", 0.},
		 {"smoothness", 1.},
		 {"oversample", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[1] == 128);
	CHECK(run.Output().Pixels[3] == 255);
}
TEST_CASE(
	"Lens Blur common masks channels mix and inactive copy retain originals", "[imagegraph][source_2d]"
) {
	const Image image = Uniform(), black = SolidImage(1, 1, {0, 0, 0, 0});
	const auto masked = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}, {"mask", &black}},
		{{"iteration", int64_t{1}}, {"strength", 0.}}
	);
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == image.Pixels);
	const auto mixed = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{1}}, {"strength", 0.}, {"mix", .5}, {"channel", int64_t{1}}}
	);
	INFO(mixed.Message);
	REQUIRE(mixed.Ok);
	CHECK(mixed.Output().Pixels[0] == 48);
	CHECK(mixed.Output().Pixels[1] == 128);
	CHECK(mixed.Output().Pixels[3] == 128);
	const auto inactive =
		RunNode("pc.blur_bokeh", {{"surface_in", &image}}, {{"active", false}, {"iteration", int64_t{0}}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Lens Blur bounds loop work and refuses selected undefined denominators", "[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	const auto zero = RunNode("pc.blur_bokeh", {{"surface_in", &image}}, {{"iteration", int64_t{0}}});
	CHECK(zero.Code == Status::UnsupportedExecution);
	CHECK(zero.Port == "iteration");
	const auto huge = RunNode("pc.blur_bokeh", {{"surface_in", &image}}, {{"iteration", int64_t{64000001}}});
	CHECK(huge.Code == Status::LimitExceeded);
	const Image empty = SolidImage(1, 1, {0, 0, 0, 0});
	const auto noWeights =
		RunNode("pc.blur_bokeh", {{"surface_in", &empty}}, {{"iteration", int64_t{1}}, {"smoothness", 0.}});
	CHECK(noWeights.Code == Status::UnsupportedExecution);
	CHECK(noWeights.Port == "smoothness");
	const auto alphaZero =
		RunNode("pc.blur_bokeh", {{"surface_in", &empty}}, {{"iteration", int64_t{1}}, {"smoothness", 1.}});
	REQUIRE(alphaZero.Ok);
	CHECK(alphaZero.Output().Pixels == empty.Pixels);
}
TEST_CASE("Lens Blur single-channel draw bypasses its shader accumulator", "[imagegraph][source_2d]") {
	Image image;
	image.Width = image.Height = 1;
	image.Format = SurfaceFormat::R8Unorm;
	image.Pixels = {64};
	const auto run = RunNode(
		"pc.blur_bokeh",
		{{"surface_in", &image}},
		{{"iteration", int64_t{0}}, {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Lens Blur graph publishes owned pixels and atomically rejects work changes", "[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{64, 128, 192, 128}}}},
		{"bokeh", "pc.blur_bokeh", "", {}, {{"iteration", int64_t{1}}, {"strength", 0.}}}
	};
	document.Links = {{"source", "surface_out", "bokeh", "surface_in"}};
	document.Outputs = {{"image", "bokeh", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result;
	const auto status = Evaluate(document, plan, "image", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{32, 64, 96, 128, 32, 64, 96, 128});
	const Image previous = result;
	CHECK(Evaluate(document, plan, "image", {}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == previous);
	document.Nodes.back().Values.front().Data = int64_t{64000001};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "image", {}, result, diagnostic) == Status::LimitExceeded);
	CHECK(result == previous);
}
TEST_CASE("Lens Blur clamps contrast factor and keeps per-channel color weights", "[imagegraph][source_2d]") {
	const Image image = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255}),
				uv = SolidImage(1, 1, {255, 255, 0, 0});
	for (double factor : {-3., 1.}) {
		const auto run = RunNode(
			"pc.blur_bokeh",
			{{"surface_in", &image}, {"uv_map", &uv}},
			{{"iteration", int64_t{2}},
			 {"strength", 0.},
			 {"contrast", 10.},
			 {"smoothness", 2.},
			 {"contrast_factor", factor},
			 {"oversample", EnumValue{3}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[0] == 219);
		CHECK(run.Output().Pixels[1] == 219);
		CHECK(run.Output().Pixels[2] == 0);
		CHECK(run.Output().Pixels[3] == 255);
	}
}
TEST_CASE(
	"Lens Blur vectorizes real iteration leaves through source integer rounding", "[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	Node iterations{"iterations", "pc.array", "", {}, {}, {}};
	iterations.DynamicInputs = {{"input_0", ValueType::Scalar, 1.}, {"input_1", ValueType::Scalar, 1.5}};
	document.Nodes = {
		iterations,
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{64, 128, 192, 128}}}},
		{"bokeh", "pc.blur_bokeh", "", {}, {{"strength", 0.}}}
	};
	document.Links = {
		{"source", "surface_out", "bokeh", "surface_in"}, {"iterations", "array", "bokeh", "iteration"}
	};
	document.Outputs = {{"image", "bokeh", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray result;
	const auto status = EvaluateArray(document, plan, "image", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 2);
	CHECK(result.Images[0].Pixels == std::vector<uint8_t>{32, 64, 96, 128, 32, 64, 96, 128});
	CHECK(result.Images[1].Pixels == result.Images[0].Pixels);
}
TEST_CASE(
	"Lens Blur source integer getter rounds fractional iteration before its shader loop",
	"[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255}),
				uv = SolidImage(1, 1, {255, 255, 0, 0});
	for (const auto &[iteration, red, green] :
		 std::array<std::array<double, 3>, 2>{{{1.1, 255, 0}, {1.5, 128, 128}}}) {
		const auto run = RunNode(
			"pc.blur_bokeh",
			{{"surface_in", &image}, {"uv_map", &uv}},
			{{"iteration", iteration},
			 {"strength", 0.},
			 {"contrast", 0.},
			 {"smoothness", 1.},
			 {"oversample", EnumValue{3}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[0] == uint8_t(red));
		CHECK(run.Output().Pixels[1] == uint8_t(green));
		CHECK(run.Output().Pixels[3] == 255);
	}
}

TEST_CASE("Lens Blur refuses source power on negative HDR samples", "[imagegraph][source_2d]") {
	Image image;
	image.Width = image.Height = 1;
	image.Format = SurfaceFormat::RGBA32Float;
	const std::array<float, 4> channels{-1.f, 0.f, 0.f, 1.f};
	image.Pixels.resize(channels.size() * sizeof(float));
	for (size_t channel = 0; channel < channels.size(); ++channel) {
		const auto bytes = std::bit_cast<std::array<uint8_t, 4>>(channels[channel]);
		for (size_t byte = 0; byte < bytes.size(); ++byte)
			image.Pixels[channel * bytes.size() + byte] = bytes[byte];
	}
	const auto run =
		RunNode("pc.blur_bokeh", {{"surface_in", &image}}, {{"iteration", int64_t{1}}, {"strength", 0.}});
	CHECK(run.Code == Status::UnsupportedExecution);
	CHECK(run.Port == "surface_in");
	Curve curve = Constant(1);
	curve.Header[1] = 0;
	const Image uniform = Uniform();
	const auto division = RunNode(
		"pc.blur_bokeh", {{"surface_in", &uniform}}, {{"strength_curved", true}, {"strength_curve", curve}}
	);
	CHECK(division.Code == Status::UnsupportedExecution);
	CHECK(division.Port == "strength_curve");
}

TEST_CASE("BokehBlur admits total processor-row work before output allocation", "[imagegraph][source_2d]") {
	const auto *entry = FindCatalogueEntry("pc.blur_bokeh");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.blur_bokeh");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"bounded", "pc.blur_bokeh", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 65536;
	const Image image = SolidImage(2, 2, {64, 128, 192, 128});
	context.Images.emplace_back("surface_in", &image);
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "iteration");
	CHECK(context.OutputImages.empty());
}
