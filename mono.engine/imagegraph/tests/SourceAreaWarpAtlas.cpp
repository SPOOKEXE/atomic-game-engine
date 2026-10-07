#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_area_warp_atlas")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Coordinates(uint32_t width = 2, uint32_t height = 2) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const size_t pixel = (size_t(y) * width + x) * 4;
				image.Pixels[pixel] = uint8_t(32 + x * 64);
				image.Pixels[pixel + 1] = uint8_t(48 + y * 64);
				image.Pixels[pixel + 2] = uint8_t(16 + (x + y) * 32);
				image.Pixels[pixel + 3] = 255;
			}
		return image;
	}
	AtlasValue SurfaceAtlas(Image surface) {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = std::move(surface);
		data.Dimension = {double(data.Surface.Data.Width), double(data.Surface.Data.Height)};
		return atlas;
	}
	void Pixel(const Image &image, uint32_t x, uint32_t y, std::array<uint8_t, 4> expected) {
		REQUIRE(x < image.Width);
		REQUIRE(y < image.Height);
		const size_t pixel = (size_t(y) * image.Width + x) * 4;
		for (size_t channel = 0; channel < expected.size(); ++channel)
			CHECK(image.Pixels[pixel + channel] == expected[channel]);
	}
	SurfacePixel FloatPixel(const Image &image, uint32_t x, uint32_t y) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
}

TEST_CASE("Area Warp uses Atlas position and scale with only the Area top-left", "[area_warp][atlas]") {
	auto atlas = SurfaceAtlas(Coordinates());
	atlas.Data->Position = {0, 0};
	atlas.Data->Scale = {2, 1};
	atlas.Data->Dimension = {100, 200};
	const auto draw = [&](Area area) {
		return RunNode(
			"pc.wrap_area", {}, {{"surface_in", atlas}, {"area", area}, {"area_unit", EnumValue{0}}}
		);
	};
	auto zeroArea = draw({0, 0, 0, 0});
	INFO(zeroArea.Message);
	REQUIRE(zeroArea.Ok);
	CHECK(zeroArea.Output().Width == 2);
	CHECK(zeroArea.Output().Height == 2);
	auto unrelatedArea = draw({7, 3, 7, 3});
	INFO(unrelatedArea.Message);
	REQUIRE(unrelatedArea.Ok);
	CHECK(unrelatedArea.Output().Pixels == zeroArea.Output().Pixels);
	Pixel(zeroArea.Output(), 0, 0, {32, 48, 16, 255});
	Pixel(zeroArea.Output(), 1, 0, {32, 48, 16, 255});
	Pixel(zeroArea.Output(), 0, 1, {32, 112, 48, 255});
	atlas.Data->Scale = {0, 1};
	auto zeroScale = RunNode("pc.wrap_area", {}, {{"surface_in", atlas}});
	REQUIRE(zeroScale.Ok);
	for (size_t pixel = 3; pixel < zeroScale.Output().Pixels.size(); pixel += 4)
		CHECK(zeroScale.Output().Pixels[pixel] == 0);
}

TEST_CASE("Area Warp adds the Area top-left to Atlas position", "[area_warp][atlas]") {
	auto atlas = SurfaceAtlas(Coordinates());
	atlas.Data->Position = {-1, 0};
	auto result = RunNode(
		"pc.wrap_area", {}, {{"surface_in", atlas}, {"area", Area{2, 0, 1, 0}}, {"area_unit", EnumValue{0}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	Pixel(result.Output(), 0, 0, {32, 48, 16, 255});
}

TEST_CASE("Area Warp uses inverse Atlas rotation at pixel centers", "[area_warp][atlas]") {
	auto atlas = SurfaceAtlas(Coordinates());
	atlas.Data->Position = {0, 2};
	atlas.Data->RotationDegrees = 90;
	auto result = RunNode("pc.wrap_area", {}, {{"surface_in", atlas}});
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(result.Output().Width == 2);
	CHECK(result.Output().Height == 2);
	Pixel(result.Output(), 0, 0, {96, 48, 48, 255});
	Pixel(result.Output(), 1, 0, {96, 112, 80, 255});
	Pixel(result.Output(), 0, 1, {32, 48, 16, 255});
	Pixel(result.Output(), 1, 1, {32, 112, 48, 255});
}

TEST_CASE("Area Warp applies RGB tint and Atlas alpha without tint alpha", "[area_warp][atlas]") {
	auto atlas = SurfaceAtlas(Coordinates(1, 1));
	atlas.Data->Blend = {128, 64, 255, 0};
	atlas.Data->Alpha = .5;
	auto result = RunNode("pc.wrap_area", {}, {{"surface_in", atlas}});
	INFO(result.Message);
	REQUIRE(result.Ok);
	Pixel(result.Output(), 0, 0, {16, 12, 16, 128});
}

TEST_CASE("Area Warp preserves signed float tint and expands red SurfaceAtlas pixels", "[area_warp][atlas]") {
	Image floating{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(floating, 0, 0, {-2, 3, .25, .5}));
	auto atlas = SurfaceAtlas(floating);
	atlas.Data->Blend = {128, 64, 255, 0};
	atlas.Data->Alpha = .5;
	auto result = RunNode("pc.wrap_area", {}, {{"surface_in", atlas}});
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(result.Output().Format == SurfaceFormat::RGBA32Float);
	const auto tinted = FloatPixel(result.Output(), 0, 0);
	CHECK(tinted[0] == Catch::Approx(-2.0 * 128 / 255));
	CHECK(tinted[1] == Catch::Approx(3.0 * 64 / 255));
	CHECK(tinted[2] == Catch::Approx(.25));
	CHECK(tinted[3] == Catch::Approx(.25));

	Image red{2, 1, {32, 192}, 0, SurfaceFormat::R8Unorm};
	auto redAtlas = SurfaceAtlas(red);
	redAtlas.Data->Blend = {128, 255, 64, 0};
	redAtlas.Data->Alpha = .5;
	result = RunNode("pc.wrap_area", {}, {{"surface_in", redAtlas}, {"attribute_color_depth", EnumValue{3}}});
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(result.Output().Format == SurfaceFormat::RGBA8Unorm);
	Pixel(result.Output(), 0, 0, {16, 32, 8, 128});
	Pixel(result.Output(), 1, 0, {96, 192, 48, 128});
}

TEST_CASE("Area Warp inactive SurfaceAtlas copies its backing surface image", "[area_warp][atlas]") {
	auto atlas = SurfaceAtlas(Coordinates());
	atlas.Data->Position = {1, 0};
	atlas.Data->Scale = {1, 1};
	atlas.Data->Blend = {255, 128, 255, 0};
	atlas.Data->Alpha = .5;
	atlas.Data->OriginalSurface = SurfaceValue{Coordinates(4, 3)};
	atlas.Data->OriginalDimension = {4, 3};
	auto result = RunNode(
		"pc.wrap_area", {}, {{"surface_in", atlas}, {"active", false}, {"area", Area{0, 0, 0, 0, 0, 9}}}
	);
	INFO(result.Message);
	REQUIRE(result.Ok);
	CHECK(result.Output().Width == 2);
	CHECK(result.Output().Height == 2);
	CHECK(result.Output().Pixels == atlas.Data->Surface.Data.Pixels);
	CHECK(result.Output().Format == atlas.Data->Surface.Data.Format);
	CHECK(atlas.Data->Surface.Data.Pixels == Coordinates().Pixels);
}

TEST_CASE("Area Warp evaluates Atlas junctions as active and inactive images", "[area_warp][atlas]") {
	auto atlas = SurfaceAtlas(Coordinates());
	atlas.Data->Position = {1, 0};
	atlas.Data->Alpha = .5;
	const AtlasValue original = atlas;
	for (bool active : {true, false}) {
		Document document;
		document.FormatVersion = 9;
		document.Junctions = {{"atlas", "", ValueType::Atlas, atlas}};
		document.Nodes = {{"warp", "pc.wrap_area", "", {}, {{"active", active}}}};
		document.Links = {{"atlas", "value", "warp", "surface_in"}};
		document.Outputs = {{"out", "warp", "surface_out"}};
		Plan plan;
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		document = std::move(restored);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Image output;
		const auto status = Evaluate(document, plan, "out", output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Width == 2);
		CHECK(output.Height == 2);
		if (active)
			Pixel(output, 1, 0, {32, 48, 16, 128});
		else
			CHECK(output.Pixels == atlas.Data->Surface.Data.Pixels);
		CHECK(std::get<AtlasValue>(document.Junctions[0].Default.value()) == original);
	}
}

TEST_CASE("Area Warp applies one active scalar to every flat Atlas row", "[area_warp][atlas]") {
	auto first = SurfaceAtlas(Coordinates());
	first.Data->Position = {1, 0};
	auto second = SurfaceAtlas(Coordinates(1, 1));
	second.Data->Position = {0, 0};
	for (bool active : {true, false}) {
		Document document;
		document.FormatVersion = 9;
		document.Junctions = {
			{"atlases", "", ValueType::Array, ArrayValue{ValueType::Atlas, {first, second}}}
		};
		document.Nodes = {{"warp", "pc.wrap_area", "", {}, {{"active", active}}}};
		document.Links = {{"atlases", "value", "warp", "surface_in"}};
		document.Outputs = {{"out", "warp", "surface_out"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray output;
		const auto status = EvaluateArray(document, plan, "out", {}, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(output.Images.size() == 2);
		if (active) {
			Pixel(output.Images[0], 1, 0, {32, 48, 16, 255});
			Pixel(output.Images[1], 0, 0, {32, 48, 16, 255});
		} else {
			CHECK(output.Images[0].Pixels == first.Data->Surface.Data.Pixels);
			CHECK(output.Images[1].Pixels == second.Data->Surface.Data.Pixels);
		}
	}
}
