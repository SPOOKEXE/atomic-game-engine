#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
TEST_SUITE_ID("engine.imagegraph.source_aniso_noise")
using namespace engine::imagegraph;
namespace {
	float Fraction(float x) {
		return x - std::floor(x);
	}
	// Independent transcription of the pinned sh_ani_noise.fsh random equations.
	float SourceRandom(float x, float y, float seed) {
		const auto endpoint = [&](float s) {
			const float t = s + 453.456f;
			return Fraction(
				std::sin((x * 12.9898f + y * 78.233f) * (t - std::floor(t / 100.f) * 100.f) * 12.588f) *
				43758.5453123f
			);
		};
		const float t = Fraction(seed);
		return endpoint(std::floor(seed)) * (1.f - t) + endpoint(std::floor(seed) + 1.f) * t;
	}
}
TEST_CASE("Aniso pinned Blend random and Waterfall equations", "[imagegraph]") {
	for (int mode = 0; mode < 2; ++mode) {
		auto run = imagegraph_test::RunNode(
			"pc.noise_aniso",
			{},
			{{"dimension", Vector2{4, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"position_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"seed_2", 71.5},
			 {"x_amount", 2.0},
			 {"y_amount", 16.0},
			 {"rotation", 0.0},
			 {"render_mode", EnumValue{mode}},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 4; ++x) {
				const float yy = std::floor((float(y) + .5f) / 4.f * 16.f),
							xx = ((float(x) + .5f) / 4.f + SourceRandom(1.f, yy, 17.25f)) * 2.f;
				const float floorX = std::floor(xx), t = xx - floorX;
				const float expected = mode == 1 ? t
												 : SourceRandom(floorX, yy, 71.5f) * (1.f - t) +
													   SourceRandom(floorX + 1.f, yy, 71.5f) * t;
				SurfacePixel pixel{};
				REQUIRE(LoadSurfacePixel(run.Output(), x, y, pixel));
				CHECK(pixel[0] == Catch::Approx(expected).margin(1e-6));
				CHECK(pixel[3] == 1);
			}
	}
}
TEST_CASE("Aniso zero X and level extrapolation are retained", "[imagegraph]") {
	auto run = imagegraph_test::RunNode(
		"pc.noise_aniso",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 1.0},
		 {"x_amount", 0.0},
		 {"render_mode", EnumValue{1}},
		 {"level_in", Vector2{.25, .75}},
		 {"level_out", Vector2{0, 1}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	SurfacePixel pixel{};
	REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, pixel));
	CHECK(pixel[0] == -.5);
}
TEST_CASE("Aniso equal source levels refuse before output", "[imagegraph]") {
	auto run = imagegraph_test::RunNode(
		"pc.noise_aniso",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"seed", 1.0},
		 {"seed_2", 2.0},
		 {"level_in", Vector2{1, 1}}}
	);
	CHECK_FALSE(run.Ok);
	CHECK(run.Port == "level_in");
	CHECK(run.Images.empty());
}
TEST_CASE("Aniso source Tile uses the strict real boolean threshold", "[imagegraph]") {
	for (double tile : {-0.1, .25, .5, .5001}) {
		auto run = imagegraph_test::RunNode(
			"pc.noise_aniso",
			{},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"seed_2", 71.5},
			 {"x_amount", 9.0},
			 {"y_amount", 16.0},
			 {"tile", tile},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		float xx = (.5f + SourceRandom(1.f, 8.f, 17.25f)) * 9.f;
		if (tile > .5) xx = Fraction(Fraction(xx / 2.f) + 1.f) * 2.f;
		const float x0 = std::floor(xx), t = xx - x0;
		const float expected =
			SourceRandom(x0, 8.f, 71.5f) * (1.f - t) + SourceRandom(x0 + 1.f, 8.f, 71.5f) * t;
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, pixel));
		CHECK(pixel[0] == Catch::Approx(expected).margin(1e-6));
	}
}
TEST_CASE("Aniso individual mapped controls sample original UV", "[imagegraph]") {
	Image map{1, 1, {128, 128, 128, 255}, 0};
	for (std::string_view port : {"x_amount_map", "y_amount_map", "rotation_map"}) {
		const bool mx = port == "x_amount_map", my = port == "y_amount_map", ma = port == "rotation_map";
		auto run = imagegraph_test::RunNode(
			"pc.noise_aniso",
			{{port, &map}},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"position_unit", EnumValue{0}},
			 {"seed", 17.25},
			 {"render_mode", EnumValue{1}},
			 {"x_amount", Vector2{2, 4}},
			 {"y_amount", Vector2{4, 8}},
			 {"rotation", Vector2{0, 90}},
			 {"x_amount_mapped", mx},
			 {"y_amount_mapped", my},
			 {"rotation_mapped", ma},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const float sample = float(128. / 255.);
		const float brightness = (sample + sample + sample) / 3.f;
		const float angle = (ma ? 90.f * brightness : 0.f) * .017453292519943295f;
		const float px = .5f * std::cos(angle) - .5f * std::sin(angle),
					py = .5f * std::sin(angle) + .5f * std::cos(angle);
		const float ny = my ? 4.f * (1.f - brightness) + 8.f * brightness : 8.f;
		const float nx = mx ? 2.f * (1.f - brightness) + 4.f * brightness : 2.f;
		const float yy = std::floor(py * ny), xx = (px + SourceRandom(1.f, yy, 17.25f)) * nx;
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, pixel));
		CHECK(pixel[0] == Catch::Approx(Fraction(xx)).margin(1e-6));
	}
}
