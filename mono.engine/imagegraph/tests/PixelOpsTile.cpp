#include "../src/PixelOpsTile.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_tile")

using engine::imagegraph::Image;
using engine::imagegraph::detail::TileControls;
using engine::imagegraph::detail::TileImage;
using engine::imagegraph::detail::TileStatus;

namespace {
	Image Source() {
		return {2, 2, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255}, 0};
	}

	std::vector<uint8_t> Reds(const Image &image) {
		std::vector<uint8_t> red;
		for (size_t index = 0; index < image.Pixels.size(); index += 4)
			red.push_back(image.Pixels[index]);
		return red;
	}
}

TEST_CASE("Tile default and flip-grid pattern select exact source pixels", "[imagegraph]") {
	const Image source = Source();
	Image output{4, 4, std::vector<uint8_t>(64), 0};
	TileControls control;
	REQUIRE(TileImage(source, output, control) == TileStatus::Ok);
	CHECK(
		Reds(output) == std::vector<uint8_t>{10, 20, 10, 20, 30, 40, 30, 40, 10, 20, 10, 20, 30, 40, 30, 40}
	);
	control.Pattern = 1;
	REQUIRE(TileImage(source, output, control) == TileStatus::Ok);
	CHECK(
		Reds(output) == std::vector<uint8_t>{10, 20, 20, 10, 30, 40, 40, 30, 30, 40, 40, 30, 10, 20, 20, 10}
	);
}

TEST_CASE("Tile polar pattern and spacing use source shader placement", "[imagegraph]") {
	const Image source = Source();
	Image output{4, 4, std::vector<uint8_t>(64), 0};
	TileControls control;
	control.Pattern = 2;
	REQUIRE(TileImage(source, output, control) == TileStatus::Ok);
	CHECK(
		Reds(output) == std::vector<uint8_t>{10, 20, 20, 40, 30, 40, 10, 30, 30, 10, 40, 30, 40, 20, 20, 10}
	);
	control.Pattern = 0;
	control.Spacing = {1.0, 1.0};
	REQUIRE(TileImage(source, output, control) == TileStatus::Ok);
	CHECK(Reds(output) == std::vector<uint8_t>{10, 20, 0, 10, 30, 40, 0, 30, 0, 0, 0, 0, 10, 20, 0, 10});
	CHECK(output.Pixels[(2 * 4 + 2) * 4 + 3] == 0);
}

TEST_CASE("Tile refuses undefined repeat and scale values", "[imagegraph]") {
	const Image source = Source();
	Image output{2, 2, std::vector<uint8_t>(16, 7), 0};
	TileControls control;
	control.Scale.X = 0.0;
	CHECK(TileImage(source, output, control) == TileStatus::UndefinedDivision);
	CHECK(output.Pixels == std::vector<uint8_t>(16, 7));
	control.Scale.X = 1.0;
	control.Spacing.X = -2.0;
	CHECK(TileImage(source, output, control) == TileStatus::UndefinedDivision);
	control.Spacing.X = 0.0;
	control.Pattern = 3;
	CHECK(TileImage(source, output, control) == TileStatus::InvalidControl);
}

TEST_CASE("Tile graph sizes relative output and diagnoses UV controls", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	Node solid;
	solid.Id = "solid";
	solid.Type = "image.solid";
	solid.Values = {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}};
	Node tile;
	tile.Id = "tile";
	tile.Type = "image.tile";
	tile.Values = {{"scaling_type", int64_t{1}}, {"amount", Vector2{2.0, 2.0}}};
	document.Nodes = {solid, tile};
	document.Links.push_back({"solid", "image", "tile", "image"});
	document.Outputs.push_back({"out", "tile", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Width == 2);
	CHECK(output.Height == 2);
	CHECK(
		output.Pixels ==
		std::vector<uint8_t>{10, 20, 30, 255, 10, 20, 30, 255, 10, 20, 30, 255, 10, 20, 30, 255}
	);
	document.Nodes[1].Values.push_back({"uv_mix", 1.0});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "uv_map");
}
