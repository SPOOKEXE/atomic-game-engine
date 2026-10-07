#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_displace")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Labels4x4() {
		std::vector<uint8_t> pixels;
		for (uint8_t y = 0; y < 4; ++y)
			for (uint8_t x = 0; x < 4; ++x)
				pixels.insert(pixels.end(), {uint8_t(10 + 10 * x + 40 * y), 0, 0, 255});
		return {4, 4, std::move(pixels), 0};
	}
	Image GrayMap(uint32_t width, uint32_t height, uint8_t value) {
		std::vector<uint8_t> pixels(size_t(width) * height * 4);
		for (size_t offset = 0; offset < pixels.size(); offset += 4)
			pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = pixels[offset + 3] = value;
		return {width, height, std::move(pixels), 0};
	}
	Image VectorMap() {
		std::vector<uint8_t> pixels(4 * 4 * 4);
		for (size_t offset = 0; offset < pixels.size(); offset += 4) {
			pixels[offset] = 128;
			pixels[offset + 1] = 0;
			pixels[offset + 3] = 255;
		}
		return {4, 4, std::move(pixels), 0};
	}
	Image AngleMap() {
		std::vector<uint8_t> pixels(4 * 4 * 4);
		for (size_t offset = 0; offset < pixels.size(); offset += 4) {
			pixels[offset + 1] = 128;
			pixels[offset + 3] = 255;
		}
		return {4, 4, std::move(pixels), 0};
	}
	uint8_t Red(const Image &image, uint32_t x, uint32_t y = 0) {
		return image.Pixels[(size_t(y) * image.Width + x) * 4];
	}
	uint8_t Alpha(const Image &image, uint32_t x, uint32_t y = 0) {
		return image.Pixels[(size_t(y) * image.Width + x) * 4 + 3];
	}
	Image RunLinear(const Image &source, const Image &map, Vector2 position = {1, 0}) {
		const auto run = RunNode(
			"pc.displace",
			{{"surface_in", &source}, {"displace_map", &map}},
			{{"position", position},
			 {"position_unit", EnumValue{0}},
			 {"mid_value", 0.},
			 {"interpolate", EnumValue{1}},
			 {"oversample", EnumValue{3}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		return run.Output();
	}
	SurfacePixel Pixel(const Image &image, uint32_t x, uint32_t y = 0) {
		SurfacePixel result{};
		REQUIRE(LoadSurfacePixel(image, x, y, result));
		return result;
	}
}

TEST_CASE(
	"Displace linear shifts one pixel and transparent map samples produce no shift", "[source_2d][displace]"
) {
	const auto source = Labels4x4();
	const auto shifted = RunLinear(source, GrayMap(4, 4, 255));
	CHECK(Red(shifted, 0, 0) == 20);
	CHECK(Red(shifted, 2, 1) == 80);
	CHECK(Red(shifted, 3, 0) == 40);
	CHECK(Alpha(shifted, 3, 0) == 255);

	const auto transparentMap = GrayMap(4, 4, 0);
	const auto unchanged = RunLinear(source, transparentMap);
	CHECK(unchanged.Pixels == source.Pixels);
}

TEST_CASE("Displace vector and angle modes use their source channel equations", "[source_2d][displace]") {
	const auto source = Labels4x4();
	const auto vectorMap = VectorMap();
	const auto vector = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &vectorMap}},
		{{"mode", EnumValue{1}},
		 {"mid_value", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(vector.Message);
	REQUIRE(vector.Ok);
	CHECK(Red(vector.Output(), 0, 0) == 30);

	const auto angleMap = AngleMap();
	const auto angle = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &angleMap}},
		{{"mode", EnumValue{2}},
		 {"mid_value", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(angle.Message);
	REQUIRE(angle.Ok);
	CHECK(Red(angle.Output(), 0, 0) == 30);
}

TEST_CASE("Displace gradient uses neighboring brightness and angle offset", "[source_2d][displace]") {
	const auto source = Labels4x4();
	Image map{4, 1, {0, 0, 0, 255, 64, 64, 64, 255, 128, 128, 128, 255, 192, 192, 192, 255}, 0};
	const auto gradient = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"mode", EnumValue{3}},
		 {"strength", .5},
		 {"mid_value", 0.},
		 {"angle_offset", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(gradient.Message);
	REQUIRE(gradient.Ok);
	CHECK(Red(gradient.Output(), 0, 0) == 20);

	const auto rotated = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"mode", EnumValue{3}},
		 {"strength", .5},
		 {"mid_value", 0.},
		 {"angle_offset", 90.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(rotated.Message);
	REQUIRE(rotated.Ok);
	CHECK(Red(rotated.Output(), 0, 0) == 50);
}

TEST_CASE(
	"Displace radial and zoom modes transform around explicit pixel midpoints", "[source_2d][displace]"
) {
	const auto source = Labels4x4();
	const auto map = GrayMap(4, 4, 255);
	const auto radial = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"mode", EnumValue{5}},
		 {"strength", 1.},
		 {"mid_value", 0.},
		 {"mid_point", Vector2{2, 2}},
		 {"mid_point_unit", EnumValue{0}},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(radial.Message);
	REQUIRE(radial.Ok);
	CHECK(Red(radial.Output(), 0, 1) == 120);

	const auto zoom = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"mode", EnumValue{6}},
		 {"strength", .5},
		 {"mid_value", 0.},
		 {"mid_point", Vector2{2, 2}},
		 {"mid_point_unit", EnumValue{0}},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(zoom.Message);
	REQUIRE(zoom.Ok);
	CHECK(Red(zoom.Output(), 0, 1) == 60);
}

TEST_CASE(
	"Displace iteration applies Mix once per pass and again to the final output", "[source_2d][displace]"
) {
	const Image source{4, 1, {40, 0, 0, 255, 80, 0, 0, 255, 120, 0, 0, 255, 160, 0, 0, 255}, 0};
	const auto map = GrayMap(4, 1, 255);
	auto options = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"blend_mode", EnumValue{3}},
		 {"mix_ratio", .5},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(options.Message);
	REQUIRE(options.Ok);
	CHECK(Red(options.Output(), 0) == 60);

	const auto iterated = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"iterate", true},
		 {"iteration", int64_t{1}},
		 {"blend_mode", EnumValue{3}},
		 {"mix_ratio", .5},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(iterated.Message);
	REQUIRE(iterated.Ok);
	CHECK(Red(iterated.Output(), 0) == 50);
}

TEST_CASE(
	"Displace reposition, fade distance, repeat, and stop-empty affect iterative samples",
	"[source_2d][displace]"
) {
	Image source{4, 1, {40, 0, 0, 255, 80, 0, 0, 255, 120, 0, 0, 255, 160, 0, 0, 0}, 0};
	const auto map = GrayMap(4, 1, 255);
	const auto makeIterated = [&](bool reposition, bool stopEmpty) {
		return RunNode(
			"pc.displace",
			{{"surface_in", &source}, {"displace_map", &map}},
			{{"position", Vector2{1, 0}},
			 {"position_unit", EnumValue{0}},
			 {"mid_value", 0.},
			 {"iterate", true},
			 {"iteration", int64_t{2}},
			 {"reposition", reposition},
			 {"fade_distance", true},
			 {"stop_empty", stopEmpty},
			 {"interpolate", EnumValue{1}},
			 {"oversample", EnumValue{3}}}
		);
	};
	const auto fixedPosition = makeIterated(false, false);
	const auto movedPosition = makeIterated(true, false);
	const auto stopAtEmpty = makeIterated(true, true);
	INFO(fixedPosition.Message << "; " << movedPosition.Message << "; " << stopAtEmpty.Message);
	REQUIRE(fixedPosition.Ok);
	REQUIRE(movedPosition.Ok);
	REQUIRE(stopAtEmpty.Ok);
	CHECK(Red(fixedPosition.Output(), 0) == 40);
	CHECK(Red(movedPosition.Output(), 0) == 60);
	CHECK(Red(stopAtEmpty.Output(), 1) == 120);
	CHECK(Alpha(stopAtEmpty.Output(), 1) == 255);

	const auto once = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"oversample", EnumValue{3}},
		 {"interpolate", EnumValue{1}}}
	);
	const auto twice = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"repeat", int64_t{2}},
		 {"oversample", EnumValue{3}},
		 {"interpolate", EnumValue{1}}}
	);
	REQUIRE(once.Ok);
	REQUIRE(twice.Ok);
	CHECK(Red(once.Output(), 0) == 80);
	CHECK(Red(twice.Output(), 0) == 120);
}

TEST_CASE(
	"Displace mapped strength and midpoint use their selected range endpoints", "[source_2d][displace]"
) {
	const Image source{4, 1, {40, 0, 0, 255, 80, 0, 0, 255, 120, 0, 0, 255, 160, 0, 0, 255}, 0};
	const auto map = GrayMap(4, 1, 255);
	const auto low = GrayMap(1, 1, 0);
	const auto high = GrayMap(1, 1, 255);
	const auto mappedStrength = [&](const Image &strengthMap) {
		return RunNode(
			"pc.displace",
			{{"surface_in", &source}, {"displace_map", &map}, {"strength_map", &strengthMap}},
			{{"position", Vector2{1, 0}},
			 {"position_unit", EnumValue{0}},
			 {"mid_value", 0.},
			 {"strength_mapped", true},
			 {"strength_map_range", Vector2{0, 1}},
			 {"interpolate", EnumValue{1}},
			 {"oversample", EnumValue{3}}}
		);
	};
	const auto zeroStrength = mappedStrength(low);
	const auto fullStrength = mappedStrength(high);
	INFO(zeroStrength.Message << "; " << fullStrength.Message);
	REQUIRE(zeroStrength.Ok);
	REQUIRE(fullStrength.Ok);
	CHECK(Red(zeroStrength.Output(), 0) == 40);
	CHECK(Red(fullStrength.Output(), 0) == 80);

	const auto mappedMid = [&](const Image &midMap) {
		return RunNode(
			"pc.displace",
			{{"surface_in", &source}, {"displace_map", &map}, {"mid_value_map", &midMap}},
			{{"position", Vector2{1, 0}},
			 {"position_unit", EnumValue{0}},
			 {"mid_value", Vector2{0, 1}},
			 {"mid_value_mapped", true},
			 {"mid_value_map_range", Vector2{0, 1}},
			 {"interpolate", EnumValue{1}},
			 {"oversample", EnumValue{3}}}
		);
	};
	const auto zeroMidpoint = mappedMid(low);
	const auto fullMidpoint = mappedMid(high);
	REQUIRE(zeroMidpoint.Ok);
	REQUIRE(fullMidpoint.Ok);
	CHECK(Red(zeroMidpoint.Output(), 0) == 80);
	CHECK(Red(fullMidpoint.Output(), 0) == 40);
}

TEST_CASE("Displace strength curve shapes the scalar displacement", "[source_2d][displace]") {
	const auto source = Labels4x4();
	const auto map = GrayMap(4, 4, 255);
	Curve zeroCurve;
	zeroCurve.Header = {0, 1, 0, 0, 0, 1};
	zeroCurve.Anchors = {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0}};
	const auto curved = RunNode(
		"pc.displace",
		{{"surface_in", &source}, {"displace_map", &map}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"strength_curved", true},
		 {"strength_curve", zeroCurve},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(curved.Message);
	REQUIRE(curved.Ok);
	CHECK(curved.Output().Pixels == source.Pixels);
}

TEST_CASE(
	"Displace missing map copies the typed input before mix and channel processing", "[source_2d][displace]"
) {
	Image source{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {2, .5, .25, 1}));
	const auto missing =
		RunNode("pc.displace", {{"surface_in", &source}}, {{"mix", 0.}, {"channel", int64_t{0}}});
	INFO(missing.Message);
	REQUIRE(missing.Ok);
	CHECK(missing.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(Pixel(missing.Output(), 0)[0] == Catch::Approx(2).margin(1e-6));

	Image mappedSource{2, 1, std::vector<uint8_t>(32), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(mappedSource, 0, 0, {.25, 0, 0, 1}));
	REQUIRE(StoreSurfacePixel(mappedSource, 1, 0, {2, 0, 0, 1}));
	const auto map = GrayMap(2, 1, 255);
	const auto active = RunNode(
		"pc.displace",
		{{"surface_in", &mappedSource}, {"displace_map", &map}},
		{{"position", Vector2{1, 0}},
		 {"position_unit", EnumValue{0}},
		 {"mid_value", 0.},
		 {"interpolate", EnumValue{1}},
		 {"oversample", EnumValue{3}}}
	);
	INFO(active.Message);
	REQUIRE(active.Ok);
	CHECK(active.Output().Format == SurfaceFormat::RGBA32Float);
	CHECK(Pixel(active.Output(), 0)[0] == Catch::Approx(1).margin(1e-6));
	const auto inactive = RunNode("pc.displace", {{"surface_in", &mappedSource}}, {{"active", false}});
	REQUIRE(inactive.Ok);
	CHECK(Pixel(inactive.Output(), 1)[0] == Catch::Approx(2).margin(1e-6));
}
