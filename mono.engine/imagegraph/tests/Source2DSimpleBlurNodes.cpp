#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>

TEST_SUITE_ID("engine.imagegraph.source_simple_blur")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Solid(uint32_t width, uint32_t height, std::array<uint8_t, 4> colour) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * 4);
		for (size_t at = 0; at < image.Pixels.size(); at += 4)
			for (size_t c = 0; c < 4; ++c)
				image.Pixels[at + c] = colour[c];
		return image;
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{2, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{64, 64, 64, 255}}}},
			{"blur", "pc.blur_simple", "", {}, {{"size", 0.}, {"gamma_correction", true}}}
		};
		document.Links = {{"source", "surface_out", "blur", "surface_in"}};
		document.Outputs = {{"out", "blur", "surface_out"}};
		return document;
	}
}
TEST_CASE("Non-Uniform Blur uniform colour is not alpha normalized", "[imagegraph][source_2d]") {
	const auto image = Solid(3, 3, {64, 128, 192, 128});
	const auto run = RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"size", 1.}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Non-Uniform Blur literal fractional weights suppress the positive diagonal", "[imagegraph][source_2d]"
) {
	for (uint32_t pixel : {6u, 18u}) {
		auto image = Solid(5, 5, {0, 0, 0, 255});
		image.Pixels[pixel * 4] = 255;
		const auto run = RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"size", 1.5}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[12 * 4] == (pixel == 6 ? 16 : 0));
	}
}
TEST_CASE("Non-Uniform Blur gamma leaves center uncorrected before accumulation", "[imagegraph][source_2d]") {
	const auto image = Solid(3, 3, {64, 64, 64, 255});
	for (double size : {0., 1.}) {
		const auto run =
			RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"size", size}, {"gamma_correction", true}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[16] == (size == 0 ? 136 : 96));
	}
}
TEST_CASE("Non-Uniform Blur mask uses average RGB times alpha", "[imagegraph][source_2d]") {
	const auto image = Solid(3, 3, {64, 64, 64, 255});
	const auto opaque = Solid(1, 1, {255, 255, 255, 255}), transparent = Solid(1, 1, {255, 255, 255, 0});
	const auto masked = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}, {"blur_mask", &transparent}},
		{{"size", 1.}, {"gamma_correction", true}}
	);
	const auto full = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}, {"blur_mask", &opaque}},
		{{"size", 1.}, {"gamma_correction", true}}
	);
	REQUIRE(masked.Ok);
	REQUIRE(full.Ok);
	CHECK(masked.Output().Pixels[16] == 136);
	CHECK(full.Output().Pixels[16] == 96);
}
TEST_CASE("Non-Uniform Blur Override Color replaces RGB and multiplies alpha", "[imagegraph][source_2d]") {
	const auto image = Solid(2, 2, {64, 128, 192, 128});
	const auto run = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}},
		{{"size", 0.}, {"override_color", true}, {"color", Colour{128, 64, 0, 128}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == Solid(2, 2, {128, 64, 0, 64}).Pixels);
}
TEST_CASE(
	"Non-Uniform Blur gradient uses preoverride brightness and positive fractional wrap",
	"[imagegraph][source_2d]"
) {
	const auto image = Solid(1, 1, {64, 128, 192, 128});
	const auto run = RunNode(
		"pc.blur_simple", {{"surface_in", &image}}, {{"size", 0.}, {"use_gradient", true}, {"shift", -.25}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{32, 64, 96, 128});
	const auto white = Solid(1, 1, {255, 255, 255, 255});
	const auto wrapped =
		RunNode("pc.blur_simple", {{"surface_in", &white}}, {{"size", 0.}, {"use_gradient", true}});
	REQUIRE(wrapped.Ok);
	CHECK(wrapped.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255});
}
TEST_CASE("Non-Uniform Blur mapped gradient samples its owned colour surface", "[imagegraph][source_2d]") {
	const auto image = Solid(1, 1, {255, 255, 255, 128}), map = Solid(1, 1, {128, 64, 32, 128});
	const auto run = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}, {"gradient_map", &map}},
		{{"size", 0.}, {"use_gradient", true}, {"gradient_mapped", true}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 64, 32, 64});
}
TEST_CASE("Non-Uniform Blur source attribute oversampling controls edge alpha", "[imagegraph][source_2d]") {
	const auto image = Solid(1, 1, {255, 255, 255, 255});
	const auto empty =
		RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"size", 1.}, {"oversample", EnumValue{1}}});
	const auto clamp = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}},
		{{"size", 1.}, {"oversample", EnumValue{3}}, {"oversample_mode", EnumValue{0}}}
	);
	REQUIRE(empty.Ok);
	REQUIRE(clamp.Ok);
	CHECK(empty.Output().Pixels == std::vector<uint8_t>{85, 85, 85, 85});
	CHECK(clamp.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Non-Uniform Blur common mask channel mix and inactive paths are retained", "[imagegraph][source_2d]"
) {
	const auto image = Solid(1, 1, {64, 128, 192, 128}), mask = Solid(1, 1, {0, 0, 0, 0});
	const auto masked = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}, {"mask", &mask}},
		{{"size", 0.}, {"override_color", true}, {"color", Colour{0, 0, 0, 255}}}
	);
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == image.Pixels);
	const auto mixed = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}},
		{{"size", 0.},
		 {"override_color", true},
		 {"color", Colour{0, 0, 0, 255}},
		 {"mix", .5},
		 {"channel", int64_t{1}}}
	);
	REQUIRE(mixed.Ok);
	CHECK(mixed.Output().Pixels == std::vector<uint8_t>{32, 128, 192, 128});
	const auto inactive =
		RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"active", false}, {"shift", 1.}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Non-Uniform Blur positive handle alias and shadow UV input require source evidence",
	"[imagegraph][source_2d]"
) {
	const auto image = Solid(1, 1, {64, 128, 192, 255}), uv = Solid(1, 1, {0, 0, 0, 0});
	const auto alias = RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"shift", .5}});
	CHECK(alias.Code == Status::UnsupportedExecution);
	CHECK(alias.Port == "shift");
	const auto shadow = RunNode("pc.blur_simple", {{"surface_in", &image}, {"uv_map", &uv}});
	CHECK(shadow.Code == Status::UnsupportedExecution);
	CHECK(shadow.Port == "uv_map");
}
TEST_CASE(
	"Non-Uniform Blur safe single-channel draw bypasses handle and gamma shader checks",
	"[imagegraph][source_2d]"
) {
	Image image;
	image.Width = image.Height = 1;
	image.Format = SurfaceFormat::R8Unorm;
	image.Pixels = {64};
	const auto run = RunNode(
		"pc.blur_simple",
		{{"surface_in", &image}},
		{{"shift", 1.}, {"gamma_correction", true}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Non-Uniform Blur actual graph keeps its prior image under byte and work refusal",
	"[imagegraph][source_2d]"
) {
	auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	const auto status = Evaluate(document, plan, "out", {}, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(image.Pixels == Solid(2, 1, {136, 136, 136, 255}).Pixels);
	const Image prior = image;
	CHECK(Evaluate(document, plan, "out", {}, image, diagnostic, 1) == Status::LimitExceeded);
	CHECK(image == prior);
	document.Nodes[1].Values[0].Data = 4001.;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", {}, image, diagnostic) == Status::LimitExceeded);
	CHECK(image == prior);
}
TEST_CASE("Non-Uniform Blur actual processor rows execute distinct size leaves", "[imagegraph][source_2d]") {
	auto document = Graph();
	Node sizes{"sizes", "pc.array", "", {}, {}, {}};
	sizes.DynamicInputs = {{"input_0", ValueType::Scalar, 0.}, {"input_1", ValueType::Scalar, 1.}};
	document.Nodes.push_back(sizes);
	document.Links.push_back({"sizes", "array", "blur", "size"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(document, plan, "out", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels == Solid(2, 1, {136, 136, 136, 255}).Pixels);
	CHECK(images.Images[1].Pixels == Solid(2, 1, {96, 96, 96, 255}).Pixels);
}
TEST_CASE(
	"Non-Uniform Blur HDR gamma and oversized shader gradient are explicit diagnostics",
	"[imagegraph][source_2d]"
) {
	Image image;
	image.Width = image.Height = 1;
	image.Format = SurfaceFormat::RGBA32Float;
	image.Pixels.resize(16);
	const std::array<float, 4> channels{-1, 0, 0, 1};
	for (size_t c = 0; c < 4; ++c) {
		const auto bytes = std::bit_cast<std::array<uint8_t, 4>>(channels[c]);
		for (size_t b = 0; b < 4; ++b)
			image.Pixels[c * 4 + b] = bytes[b];
	}
	const auto run =
		RunNode("pc.blur_simple", {{"surface_in", &image}}, {{"size", 0.}, {"gamma_correction", true}});
	CHECK(run.Code == Status::UnsupportedExecution);
	Gradient gradient;
	gradient.Keys.resize(65, {0, Colour{255, 255, 255, 255}});
	const auto uniform = Solid(1, 1, {255, 255, 255, 255});
	const auto large = RunNode(
		"pc.blur_simple", {{"surface_in", &uniform}}, {{"use_gradient", true}, {"gradient", gradient}}
	);
	CHECK(large.Code == Status::UnsupportedExecution);
	CHECK(large.Port == "gradient");
}
