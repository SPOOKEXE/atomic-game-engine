// Exact pixel fixtures for filter catalogue executors. Expected values are derived by hand from the pinned
// source shader named in each case.

#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.node_filter")

using namespace engine::imagegraph;
using imagegraph_test::MakeImage;
using imagegraph_test::RunNode;

TEST_CASE("BW thresholds Rec. 709 luma after brightness and contrast", "[imagegraph]") {
	// (20/255 + 0.1) * 1.5 = 0.268 is dark; (100/255 + 0.1) * 1.5 has luma 0.738 and is light.
	const Image source = MakeImage(2, 1, {20, 20, 20, 128, 100, 100, 100, 255});
	const auto run = RunNode("pc.bw", {{"surface_in", &source}}, {{"brightness", 0.1}, {"contrast", 1.5}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 128, 255, 255, 255, 255});
}

TEST_CASE("BW mapped brightness mixes its range by the map mean", "[imagegraph]") {
	const Image source = MakeImage(2, 1, {20, 20, 20, 255, 20, 20, 20, 255});
	// A white texel selects the range high end 0.5, a black texel the low end 0.
	const Image map = MakeImage(2, 1, {255, 255, 255, 255, 0, 0, 0, 255});
	const auto run = RunNode(
		"pc.bw",
		{{"surface_in", &source}, {"brightness_map", &map}},
		{{"brightness_mapped", true}, {"brightness_map_range", Vector2{0.0, 0.5}}, {"contrast", 1.5}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255});
	// The map is ignored while its toggle is off.
	const auto unmapped = RunNode(
		"pc.bw",
		{{"surface_in", &source}, {"brightness_map", &map}},
		{{"brightness_map_range", Vector2{0.0, 0.5}}, {"contrast", 1.5}}
	);
	REQUIRE(unmapped.Ok);
	CHECK(unmapped.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 0, 0, 0, 255});
}

TEST_CASE("Processor Mix, Channel and Active follow the source post-process", "[imagegraph]") {
	const Image source = MakeImage(1, 1, {20, 40, 60, 128});
	const auto mixed = RunNode("pc.bw", {{"surface_in", &source}}, {{"mix", 0.5}});
	REQUIRE(mixed.Ok);
	CHECK(mixed.Output().Pixels == std::vector<uint8_t>{10, 20, 30, 128});
	// Channel 0b0001 keeps only the edited red channel.
	const auto red = RunNode("pc.bw", {{"surface_in", &source}}, {{"channel", int64_t{1}}});
	REQUIRE(red.Ok);
	CHECK(red.Output().Pixels == std::vector<uint8_t>{0, 40, 60, 128});
	const auto inactive = RunNode("pc.bw", {{"surface_in", &source}}, {{"active", false}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == source.Pixels);
	const auto missing = RunNode("pc.bw", {});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Code == Status::InvalidValue);
	CHECK(missing.Port == "surface_in");
}

TEST_CASE("BW evaluates through a document graph", "[imagegraph]") {
	Document document;
	document.FormatVersion = 6;
	document.Nodes.push_back(
		{"fill",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{200, 200, 200, 90}}}}
	);
	document.Nodes.push_back({"bw", "pc.bw", "", {}, {{"brightness", -0.5}}});
	document.Links.push_back({"fill", "image", "bw", "surface_in"});
	document.Outputs.push_back({"final", "bw", "surface_out"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "final", image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	// 200/255 - 0.5 = 0.284 is below the 0.5 luma threshold.
	CHECK(image.Pixels == std::vector<uint8_t>{0, 0, 0, 90, 0, 0, 0, 90, 0, 0, 0, 90, 0, 0, 0, 90});
	CHECK(image.Hash != 0);
}

TEST_CASE("Greyscale multiplies Rec. 709 luma by alpha", "[imagegraph]") {
	// (0.2126 * 100 + 0.7152 * 150 + 0.0722 * 200) / 255 * 128 / 255 = 0.2814.
	const Image source = MakeImage(1, 1, {100, 150, 200, 128});
	const auto run = RunNode("pc.greyscale", {{"surface_in", &source}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{72, 72, 72, 128});
}

TEST_CASE("Invert flips RGB and optionally alpha", "[imagegraph]") {
	const Image source = MakeImage(1, 1, {100, 150, 200, 128});
	const auto rgb = RunNode("pc.invert", {{"surface_in", &source}});
	REQUIRE(rgb.Ok);
	CHECK(rgb.Output().Pixels == std::vector<uint8_t>{155, 105, 55, 128});
	const auto all = RunNode("pc.invert", {{"surface_in", &source}}, {{"include_alpha", true}});
	REQUIRE(all.Ok);
	CHECK(all.Output().Pixels == std::vector<uint8_t>{155, 105, 55, 127});
}

TEST_CASE("Alpha Cutoff clears pixels below Minimum inclusive of the edge", "[imagegraph]") {
	const Image source = MakeImage(3, 1, {10, 20, 30, 50, 10, 20, 30, 51, 10, 20, 30, 255});
	// The source default Minimum is 0.2 = 51 / 255.
	const auto run = RunNode("pc.alpha_cutoff", {{"surface_in", &source}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0, 10, 20, 30, 51, 10, 20, 30, 255});
}

TEST_CASE("Alpha Grey writes opaque alpha levels and ignores its Curve input", "[imagegraph]") {
	const Image source = MakeImage(1, 1, {1, 2, 3, 64});
	const auto run = RunNode("pc.alpha_grey", {{"surface_in", &source}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	Curve flat{{0, 1, 0, 0, 1, 0}, {{0, 0, 0, 0.5, 0, 0}, {0, 0, 1, 0.5, 0, 0}}};
	const auto inverted =
		RunNode("pc.alpha_grey", {{"surface_in", &source}}, {{"invert", true}, {"curve", flat}});
	REQUIRE(inverted.Ok);
	CHECK(inverted.Output().Pixels == std::vector<uint8_t>{191, 191, 191, 255});
}

TEST_CASE("Flip mirrors by axis bits", "[imagegraph]") {
	const Image source = MakeImage(2, 2, {1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255});
	const auto both = RunNode("pc.flip", {{"surface_in", &source}}, {{"axis", int64_t{3}}});
	REQUIRE(both.Ok);
	CHECK(both.Output().Pixels == std::vector<uint8_t>{4, 0, 0, 255, 3, 0, 0, 255, 2, 0, 0, 255, 1, 0, 0, 255});
	// The source default Axis is 1, a horizontal mirror.
	const auto horizontal = RunNode("pc.flip", {{"surface_in", &source}});
	REQUIRE(horizontal.Ok);
	CHECK(horizontal.Output().Pixels == std::vector<uint8_t>{2, 0, 0, 255, 1, 0, 0, 255, 4, 0, 0, 255, 3, 0, 0, 255});
}

TEST_CASE("Level remaps channel ranges before the White range", "[imagegraph]") {
	// Red 0.4 in [0.2, 0.6] becomes 0.5; White out [0, 0.5] then halves RGB.
	const Image source = MakeImage(1, 1, {102, 51, 0, 255});
	const auto run = RunNode(
		"pc.level", {{"surface_in", &source}}, {{"red_in", Vector2{0.2, 0.6}}, {"white_out", Vector2{0.0, 0.5}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 26, 0, 255});
}

TEST_CASE("Tonemap ACE applies the filmic curve to RGB only", "[imagegraph]") {
	const Image source = MakeImage(1, 1, {128, 255, 0, 77});
	const auto run = RunNode("pc.tonemap_ace", {{"surface_in", &source}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{157, 205, 0, 77});
}

TEST_CASE("Background composites the surface over Color and becomes opaque", "[imagegraph]") {
	const Image source = MakeImage(1, 1, {200, 100, 50, 128});
	const auto run = RunNode("pc.background", {{"surface_in", &source}}, {{"color", Colour{10, 20, 30, 255}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{105, 60, 40, 255});
}

TEST_CASE("Offset wraps by the negated rotated offset", "[imagegraph]") {
	const Image source = MakeImage(4, 1, {1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255});
	const auto run = RunNode("pc.offset", {{"surface_in", &source}}, {{"x_offset", 0.25}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{4, 0, 0, 255, 1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255});
}

TEST_CASE("Normal encodes the luma gradient and flat regions face the viewer", "[imagegraph]") {
	const Image flat = MakeImage(2, 2, std::vector<uint8_t>(16, 90));
	const auto still = RunNode("pc.normal", {{"surface_in", &flat}});
	REQUIRE(still.Ok);
	CHECK(still.Output().Pixels == std::vector<uint8_t>{128, 128, 255, 90, 128, 128, 255, 90, 128, 128, 255, 90, 128, 128, 255, 90});
	// Centre luma 128/255 between 0 and 1 gives n = (1, 0); Flip X negates it, then (-1, 0, 1) normalizes.
	// Repeat XY oversampling makes the single row its own vertical neighbour, so the Y slope is 0.
	const Image ramp = MakeImage(3, 1, {0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255});
	const auto slope = RunNode("pc.normal", {{"surface_in", &ramp}});
	REQUIRE(slope.Ok);
	CHECK(std::vector<uint8_t>(slope.Output().Pixels.begin() + 4, slope.Output().Pixels.begin() + 8) ==
		  std::vector<uint8_t>{37, 128, 218, 255});
}

TEST_CASE("FXAA leaves flat colour unchanged and reports no difference", "[imagegraph]") {
	const Image flat = MakeImage(3, 3, [] {
		std::vector<uint8_t> pixels;
		for (int index = 0; index < 9; index++)
			pixels.insert(pixels.end(), {40, 80, 120, 255});
		return pixels;
	}());
	const auto run = RunNode("pc.fxaa", {{"surface_in", &flat}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == flat.Pixels);
	const Image &difference = run.Output("mask");
	REQUIRE(difference.Width == 3);
	for (size_t offset = 0; offset < difference.Pixels.size(); offset += 4)
		CHECK(std::vector<uint8_t>(difference.Pixels.begin() + offset, difference.Pixels.begin() + offset + 4) ==
			  std::vector<uint8_t>{0, 0, 0, 255});
}

TEST_CASE("Outline draws an outside border around filled pixels", "[imagegraph]") {
	std::vector<uint8_t> pixels(36, 0);
	pixels[16] = 255;
	pixels[19] = 255;
	const Image source = MakeImage(3, 3, pixels);
	const auto run = RunNode("pc.outline", {{"surface_in", &source}}, {{"width", 1.0}});
	REQUIRE(run.Ok);
	for (size_t pixel = 0; pixel < 9; pixel++) {
		INFO(pixel);
		const std::vector<uint8_t> result(run.Output().Pixels.begin() + pixel * 4, run.Output().Pixels.begin() + pixel * 4 + 4);
		const std::vector<uint8_t> border(
			run.Output("outline").Pixels.begin() + pixel * 4, run.Output("outline").Pixels.begin() + pixel * 4 + 4
		);
		if (pixel == 4) {
			CHECK(result == std::vector<uint8_t>{255, 0, 0, 255});
			CHECK(border == std::vector<uint8_t>{0, 0, 0, 0});
		} else {
			CHECK(result == std::vector<uint8_t>{255, 255, 255, 255});
			CHECK(border == std::vector<uint8_t>{255, 255, 255, 255});
		}
	}
	// Width 0 takes the shader's four-neighbour branch: edge neighbours light, corners stay clear.
	const auto thin = RunNode("pc.outline", {{"surface_in", &source}});
	REQUIRE(thin.Ok);
	const std::vector<uint8_t> white{255, 255, 255, 255}, clear{0, 0, 0, 0};
	std::vector<uint8_t> expected;
	for (const auto *cell : {&clear, &white, &clear, &white, &clear, &white, &clear, &white, &clear})
		expected.insert(expected.end(), cell->begin(), cell->end());
	std::copy_n(source.Pixels.begin() + 16, 4, expected.begin() + 16);
	CHECK(thin.Output().Pixels == expected);
}

TEST_CASE("Texture Remap samples the surface at the map's flipped RG position", "[imagegraph]") {
	const Image source = MakeImage(2, 2, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255});
	// Red holds u and green holds 1 - v, so the map is an identity lookup.
	const Image identity = MakeImage(2, 2, {64, 191, 0, 255, 191, 191, 0, 255, 64, 64, 0, 255, 191, 64, 0, 255});
	const auto same = RunNode("pc.texture_remap", {{"surface_in", &source}, {"rg_map", &identity}});
	REQUIRE(same.Ok);
	CHECK(same.Output().Pixels == source.Pixels);
	// Swapping the map's columns mirrors the result.
	const Image mirrored = MakeImage(2, 2, {191, 191, 0, 255, 64, 191, 0, 255, 191, 64, 0, 255, 64, 64, 0, 255});
	const auto flip = RunNode("pc.texture_remap", {{"surface_in", &source}, {"rg_map", &mirrored}});
	REQUIRE(flip.Ok);
	CHECK(flip.Output().Pixels == std::vector<uint8_t>{20, 0, 0, 255, 10, 0, 0, 255, 40, 0, 0, 255, 30, 0, 0, 255});
}

TEST_CASE("Gamma Map, Grey Alpha and channel nodes follow their shaders", "[imagegraph]") {
	const Image grey = MakeImage(1, 1, {128, 128, 128, 255});
	const auto gamma = RunNode("pc.gamma_map", {{"surface_in", &grey}});
	REQUIRE(gamma.Ok);
	CHECK(gamma.Output().Pixels == std::vector<uint8_t>{186, 186, 186, 255});
	const Image shades = MakeImage(2, 1, {255, 255, 255, 255, 0, 0, 0, 255});
	const auto alpha = RunNode("pc.grey_alpha", {{"surface_in", &shades}});
	REQUIRE(alpha.Ok);
	CHECK(alpha.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 255, 255, 255, 0});
	const Image red = MakeImage(1, 1, {200, 0, 0, 255});
	const auto channel = RunNode("pc.combine_rgb", {{"red", &red}});
	REQUIRE(channel.Ok);
	CHECK(channel.Output().Pixels == std::vector<uint8_t>{200, 0, 0, 255});
	const auto brightness = RunNode("pc.combine_rgb", {{"red", &red}}, {{"sampling_type", EnumValue{1}}});
	REQUIRE(brightness.Ok);
	CHECK(brightness.Output().Pixels == std::vector<uint8_t>{67, 0, 0, 255});
	const Image base = MakeImage(1, 1, {10, 20, 30, 40});
	const Image level = MakeImage(1, 1, {90, 90, 90, 255});
	const auto overridden = RunNode("pc.override_channel", {{"surface", &base}, {"red", &level}});
	REQUIRE(overridden.Ok);
	CHECK(overridden.Output().Pixels == std::vector<uint8_t>{90, 20, 30, 40});
	const Image black = MakeImage(1, 1, {0, 0, 0, 255}), white = MakeImage(1, 1, {255, 255, 255, 255});
	const auto hsv = RunNode("pc.combine_hsv", {{"hue", &black}, {"saturation", &white}, {"value", &white}});
	REQUIRE(hsv.Ok);
	CHECK(hsv.Output().Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	const auto hsl = RunNode(
		"pc.combine_hsv", {{"hue", &black}, {"saturation", &white}, {"value", &grey}}, {{"color_space", EnumValue{1}}}
	);
	REQUIRE(hsl.Ok);
	CHECK(hsl.Output().Pixels == std::vector<uint8_t>{255, 1, 1, 255});
}

TEST_CASE("Curve nodes keep identity curves and apply edited ones", "[imagegraph]") {
	const Image source = MakeImage(1, 1, {40, 120, 200, 255});
	const auto identity = RunNode("pc.curve", {{"surface_in", &source}});
	REQUIRE(identity.Ok);
	CHECK(identity.Output().Pixels == source.Pixels);
	// A flat 0.5 red curve sets red to 128 before the identity Brightness curve rescales nothing.
	const Curve half{{0, 1, 0, 0, 1, 0}, {{0, 0, 0, 0.5, 0, 0}, {0, 0, 1, 0.5, 0, 0}}};
	const auto red = RunNode("pc.curve", {{"surface_in", &source}}, {{"red", half}});
	REQUIRE(red.Ok);
	CHECK(red.Output().Pixels == std::vector<uint8_t>{128, 120, 200, 255});
	const auto hsv = RunNode("pc.curve_hsv", {{"surface_in", &source}});
	REQUIRE(hsv.Ok);
	CHECK(hsv.Output().Pixels == source.Pixels);
	// A zero saturation curve turns the colour into its value grey.
	const Curve zero{{0, 1, 0, 0, 1, 0}, {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0}}};
	const auto flat = RunNode("pc.curve_hsv", {{"surface_in", &source}}, {{"saturation", zero}});
	REQUIRE(flat.Ok);
	CHECK(flat.Output().Pixels == std::vector<uint8_t>{200, 200, 200, 255});
}

TEST_CASE("Colorize maps luma times alpha through the gradient", "[imagegraph]") {
	const Image source = MakeImage(2, 1, {255, 255, 255, 128, 64, 64, 64, 255});
	const auto run = RunNode("pc.colorize", {{"surface_in", &source}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 128, 128, 128, 64, 64, 64, 255});
}

TEST_CASE("Posterize snaps to the palette or steps the inverted global range", "[imagegraph]") {
	const Image source = MakeImage(2, 1, {200, 200, 200, 255, 40, 40, 40, 255});
	const auto palette = RunNode("pc.posterize", {{"surface_in", &source}});
	REQUIRE(palette.Ok);
	CHECK(palette.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255});
	// The global range keeps minimum 1 and maximum 0, so channels invert, step and invert back.
	const Image levels = MakeImage(2, 1, {51, 51, 51, 255, 153, 153, 153, 255});
	const auto steps = RunNode("pc.posterize", {{"surface_in", &levels}}, {{"use_palette", false}});
	REQUIRE(steps.Ok);
	CHECK(steps.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 170, 170, 170, 255});
}

TEST_CASE("Threshold, Palette Shift, Average and Convolution follow their shaders", "[imagegraph]") {
	const Image levels = MakeImage(2, 1, {200, 200, 200, 255, 40, 40, 40, 255});
	const auto threshold = RunNode("pc.threshold", {{"surface_in", &levels}}, {{"brightness", true}});
	REQUIRE(threshold.Ok);
	CHECK(threshold.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255});
	const Image white = MakeImage(1, 1, {255, 255, 255, 255});
	const auto shifted = RunNode("pc.palette_shift", {{"surface_in", &white}}, {{"shift", 1.0}});
	REQUIRE(shifted.Ok);
	CHECK(shifted.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	const Image pair = MakeImage(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
	const auto average = RunNode("pc.average", {{"surface_in", &pair}});
	REQUIRE(average.Ok);
	CHECK(average.Output().Pixels == std::vector<uint8_t>{128, 0, 128, 255, 128, 0, 128, 255});
	CHECK(*average.OutputValue("color") == Value{Colour{128, 0, 128, 255}});
	const MatrixValue identity{3, 3, {0, 0, 0, 0, 1, 0, 0, 0, 0}};
	const auto convolved = RunNode("pc.convolution", {{"surface_in", &pair}}, {{"kernel", identity}});
	REQUIRE(convolved.Ok);
	CHECK(convolved.Output().Pixels == pair.Pixels);
}

TEST_CASE("Level Selector, Skew, Barrel, Spherize and Mirror follow their shaders", "[imagegraph]") {
	const Image levels = MakeImage(2, 1, {26, 26, 26, 255, 200, 200, 200, 255});
	// Luma 0.1 is within 0.1 of midpoint 0.05; luma 0.78 is not.
	const auto selected = RunNode("pc.level_selector", {{"surface_in", &levels}}, {{"midpoint", 0.05}});
	REQUIRE(selected.Ok);
	CHECK(selected.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255});
	const Image row = MakeImage(4, 1, {1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255});
	const auto still = RunNode("pc.skew", {{"surface_in", &row}});
	REQUIRE(still.Ok);
	CHECK(still.Output().Pixels == row.Pixels);
	// Intensity 1 keeps the radius, so the barrel distortion is an identity.
	const auto barrel = RunNode("pc.barrel_distort", {{"surface_in", &row}}, {{"intensity", 1.0}});
	REQUIRE(barrel.Ok);
	CHECK(barrel.Output().Pixels == row.Pixels);
	// Zero strength samples the unmoved point; depth stays above the trim everywhere.
	const auto sphere = RunNode("pc.spherize", {{"surface_in", &row}}, {{"strength", 0.0}, {"radius", 1.0}});
	REQUIRE(sphere.Ok);
	CHECK(sphere.Output().Pixels == row.Pixels);
	// A 90 degree line through the centre reflects the left half from the right: (4, 3, 3, 4).
	const auto mirrored = RunNode("pc.mirror", {{"surface_in", &row}}, {{"angle", 90.0}});
	REQUIRE(mirrored.Ok);
	CHECK(mirrored.Output().Pixels == std::vector<uint8_t>{4, 0, 0, 255, 3, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255});
	CHECK(mirrored.Output("mirror_mask").Pixels ==
		  std::vector<uint8_t>{255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255});
	const auto clean = RunNode("pc.skew", {{"surface_in", &row}}, {{"interpolate", EnumValue{6}}});
	CHECK_FALSE(clean.Ok);
	CHECK(clean.Port == "interpolate");
}

TEST_CASE("Dilate, Downscale, Stretch and Box Blur follow their shaders", "[imagegraph]") {
	const Image square = MakeImage(2, 2, {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255});
	const auto dilate = RunNode("pc.dilate", {{"surface_in", &square}}, {{"strength", 0.0}});
	REQUIRE(dilate.Ok);
	CHECK(dilate.Output().Pixels == square.Pixels);
	// A 2x downscale averages the four opaque texels: one red and one blue texel give 255 / 4 = 64 each.
	const auto down = RunNode("pc.downscale", {{"surface_in", &square}}, {{"downscale", 2.0}});
	REQUIRE(down.Ok);
	CHECK(down.Output().Width == 1);
	CHECK(down.Output().Pixels == std::vector<uint8_t>{64, 0, 64, 255});
	const auto stretch = RunNode("pc.stretch", {{"surface_in", &square}});
	REQUIRE(stretch.Ok);
	CHECK(stretch.Output().Pixels == square.Pixels);
	const auto box = RunNode("pc.blur_box", {{"surface_in", &square}}, {{"size", 0.0}});
	REQUIRE(box.Ok);
	CHECK(box.Output().Pixels == square.Pixels);
}

TEST_CASE("Chromatic Aberration Scale splits red and blue along the radial offset", "[imagegraph]") {
	// sh_chromatic_aberration on 3x1 with the centre at 1.5 px: pixel 0 has co.x = -2/3, so
	// pp.x = (4/9)(-2/3)(4)(1/3) = -0.395. Red reads u = 0.562 (pixel 1), blue wraps from -0.228 to
	// 0.772 (pixel 2) under Repeat XY. Pixel 1 sits on the centre and pixel 2 mirrors pixel 0.
	const Image source = MakeImage(3, 1, {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255});
	const auto run = RunNode("pc.chromatic_aberration", {{"surface_in", &source}}, {{"strength", 4.0}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{40, 20, 90, 255, 40, 50, 60, 255, 40, 80, 30, 255});
}

TEST_CASE("Chromatic Aberration Scale premultiplies and sums three alphas", "[imagegraph]") {
	// Flat colour: every tap is (200, 100, 50) * 128/255 with alpha 3 * 128/255, clamped to 1.
	const Image source = MakeImage(1, 1, {200, 100, 50, 128});
	const auto full = RunNode("pc.chromatic_aberration", {{"surface_in", &source}});
	REQUIRE(full.Ok);
	CHECK(full.Output().Pixels == std::vector<uint8_t>{100, 50, 25, 255});
	// Intensity 0 keeps the premultiplied centre tap.
	const auto none = RunNode("pc.chromatic_aberration", {{"surface_in", &source}}, {{"intensity", 0.0}});
	REQUIRE(none.Ok);
	CHECK(none.Output().Pixels == std::vector<uint8_t>{100, 50, 25, 128});
}

TEST_CASE("Chromatic Aberration Continuous weighs taps by the Zucconi spectrum", "[imagegraph]") {
	// Resolution 1 takes i = 0 and 1, both at spectral_zucconi6(0) = (0, 0, 0.026075). White gives
	// b = (2 * 0.026075 / 0.23)^(1/2.2) = 0.5094, and alpha is 1.
	const Image source = MakeImage(1, 1, {255, 255, 255, 255});
	const auto run = RunNode(
		"pc.chromatic_aberration", {{"surface_in", &source}}, {{"type", int64_t{1}}, {"resolution", int64_t{1}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 130, 255});
}

TEST_CASE("Chromatic Aberration Gradient weighs taps by the shifted gradient", "[imagegraph]") {
	// The default black-to-white gradient at pfract(i + 0.5) = 0.5 is grey 0.5 for both taps, so each
	// channel is (2 * (50/255)^2.2 * 0.5 / norm)^(1/2.2) with norm (0.386, 0.372, 0.23).
	const Image source = MakeImage(1, 1, {50, 50, 50, 255});
	const auto run = RunNode(
		"pc.chromatic_aberration",
		{{"surface_in", &source}},
		{{"type", int64_t{2}}, {"resolution", int64_t{1}}, {"shift", 0.5}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{77, 78, 98, 255});
	const auto zero = RunNode(
		"pc.chromatic_aberration", {{"surface_in", &source}}, {{"type", int64_t{2}}, {"resolution", int64_t{0}}}
	);
	CHECK_FALSE(zero.Ok);
}
