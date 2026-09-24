#include "../src/PixelOpsGenerate.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_generate")

using engine::imagegraph::Colour;
using engine::imagegraph::Image;

namespace {
	engine::imagegraph::Node SolidNode(std::string id, Colour colour, int64_t width = 1, int64_t height = 1) {
		return {
			std::move(id), "image.solid", "", {}, {{"width", width}, {"height", height}, {"colour", colour}}
		};
	}
}

TEST_CASE("Solid graph resolves foreground mask and mask dimensions", "[imagegraph]") {
	engine::imagegraph::Document graph;
	graph.Nodes = {
		SolidNode("foreground", {200, 100, 50, 128}),
		SolidNode("mask", {255, 255, 255, 128}),
		SolidNode("solid", {100, 50, 0, 128}, 2, 1),
	};
	graph.Links = {
		{"foreground", "image", "solid", "foreground"},
		{"mask", "image", "solid", "mask"},
	};
	graph.Outputs = {{"out", "solid", "image"}};
	engine::imagegraph::Plan plan;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
	Image image;
	REQUIRE(
		engine::imagegraph::Evaluate(graph, plan, "out", image, diagnostic) == engine::imagegraph::Status::Ok
	);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK(image.Pixels == std::vector<uint8_t>{150, 75, 25, 128});
	const uint64_t hash = image.Hash;
	REQUIRE(
		engine::imagegraph::Evaluate(graph, plan, "out", image, diagnostic) == engine::imagegraph::Status::Ok
	);
	CHECK(image.Hash == hash);
	graph.Nodes[2].Values.push_back({"use_mask_dimension", false});
	REQUIRE(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
	REQUIRE(
		engine::imagegraph::Evaluate(graph, plan, "out", image, diagnostic) == engine::imagegraph::Status::Ok
	);
	CHECK(image.Width == 2);
	CHECK(image.Pixels == std::vector<uint8_t>{150, 75, 25, 128, 150, 75, 25, 128});
}

TEST_CASE("Height Blend graph validates controls and reports undefined divisions", "[imagegraph]") {
	engine::imagegraph::Document graph;
	graph.Nodes = {
		SolidNode("background", {64, 64, 64, 255}),
		SolidNode("foreground", {128, 128, 128, 255}),
		{"blend", "image.height_blend", "", {}, {}},
	};
	graph.Links = {
		{"background", "image", "blend", "background"},
		{"foreground", "image", "blend", "foreground"},
	};
	graph.Outputs = {{"out", "blend", "image"}};
	engine::imagegraph::Plan plan;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
	Image image;
	REQUIRE(
		engine::imagegraph::Evaluate(graph, plan, "out", image, diagnostic) == engine::imagegraph::Status::Ok
	);
	CHECK(image.Pixels == std::vector<uint8_t>{141, 141, 141, 255});
	graph.Nodes[2].Values = {{"type", int64_t{0}}, {"factor", 0.0}};
	REQUIRE(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(
		engine::imagegraph::Evaluate(graph, plan, "out", image, diagnostic) ==
		engine::imagegraph::Status::UnsupportedExecution
	);
	graph.Nodes[2].Values[1].Data = 1.1;
	CHECK(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::InvalidValue);
}

TEST_CASE("Solid composes foreground before mask and Empty bypasses colour", "[imagegraph]") {
	Image output{2, 1, std::vector<uint8_t>(8), 0};
	const Image foreground{2, 1, {200, 100, 50, 128, 9, 8, 7, 0}, 0};
	const Image mask{2, 1, {255, 255, 255, 128, 0, 0, 0, 255}, 0};
	engine::imagegraph::detail::GenerateSolid(
		output, Colour{100, 50, 0, 128}, &foreground, &mask, false, false
	);
	CHECK(output.Pixels == std::vector<uint8_t>{150, 75, 25, 128, 100, 50, 0, 0});
	engine::imagegraph::detail::GenerateSolid(
		output, Colour{100, 50, 0, 128}, &foreground, &mask, false, true
	);
	CHECK(output.Pixels == std::vector<uint8_t>{150, 75, 25, 128, 100, 50, 0, 255});
	engine::imagegraph::detail::GenerateSolid(
		output, Colour{100, 50, 0, 128}, &foreground, &mask, true, false
	);
	CHECK(output.Pixels == foreground.Pixels);
	engine::imagegraph::detail::GenerateSolid(output, Colour{100, 50, 0, 128}, nullptr, nullptr, true, false);
	CHECK(output.Pixels == std::vector<uint8_t>(8));
}

TEST_CASE("Solid samples smaller foreground and mask across output extent", "[imagegraph]") {
	Image output{4, 1, std::vector<uint8_t>(16), 0};
	const Image foreground{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0};
	const Image mask{2, 1, {255, 255, 255, 255, 0, 0, 0, 255}, 0};
	engine::imagegraph::detail::GenerateSolid(
		output, Colour{0, 255, 0, 255}, &foreground, &mask, false, false
	);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0, 255, 0});
}

TEST_CASE("Height Blend matches source formula over every mode and type", "[imagegraph]") {
	const Image background{1, 1, {64, 64, 64, 255}, 0};
	const Image foreground{1, 1, {128, 128, 128, 255}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	constexpr std::array<std::array<uint8_t, 6>, 2> expected{{
		{{226, 141, 138, 226, 226, 225}},
		{{0, 24, 27, 0, 0, 0}},
	}};
	for (int64_t mode = 0; mode < 2; mode++) {
		for (int64_t type = 0; type < 6; type++) {
			REQUIRE(engine::imagegraph::detail::BlendHeight(background, foreground, output, mode, type, 0.5));
			const uint8_t grey = expected[static_cast<size_t>(mode)][static_cast<size_t>(type)];
			CHECK(output.Pixels == std::vector<uint8_t>{grey, grey, grey, 255});
		}
	}
}

TEST_CASE("Height Blend reports undefined source divisions", "[imagegraph]") {
	const Image background{1, 1, {64, 64, 64, 255}, 0};
	const Image foreground{1, 1, {128, 128, 128, 255}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	CHECK(engine::imagegraph::detail::BlendHeight(background, foreground, output, 0, 1, 0.0));
	CHECK_FALSE(engine::imagegraph::detail::BlendHeight(background, foreground, output, 0, 0, 0.0));
	CHECK_FALSE(engine::imagegraph::detail::BlendHeight(background, foreground, output, 0, 2, 0.0));
	CHECK_FALSE(engine::imagegraph::detail::BlendHeight(background, foreground, output, 0, 3, 0.0));
	CHECK_FALSE(engine::imagegraph::detail::BlendHeight(background, foreground, output, 0, 4, 0.0));
	CHECK_FALSE(engine::imagegraph::detail::BlendHeight(background, foreground, output, 0, 5, 0.0));
}
