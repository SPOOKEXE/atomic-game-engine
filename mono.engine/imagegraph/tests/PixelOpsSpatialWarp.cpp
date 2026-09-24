#include "../src/PixelOpsSpatialWarp.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_spatial_warp")

using engine::imagegraph::Image;
using engine::imagegraph::detail::BarrelControl;
using engine::imagegraph::detail::ChromaticControl;
using engine::imagegraph::detail::DilateControl;
using engine::imagegraph::detail::MirrorControl;
using engine::imagegraph::detail::RenderBarrel;
using engine::imagegraph::detail::RenderChromaticScale;
using engine::imagegraph::detail::RenderDilate;
using engine::imagegraph::detail::RenderMirror;
using engine::imagegraph::detail::RenderSpherize;
using engine::imagegraph::detail::SpatialBoundary;
using engine::imagegraph::detail::SpatialWarpStatus;
using engine::imagegraph::detail::SpherizeControl;

namespace {
	Image Grid(uint32_t size) {
		Image source;
		source.Width = source.Height = size;
		source.Pixels.resize(size_t(size) * size * 4);
		for (uint32_t y = 0; y < size; ++y)
			for (uint32_t x = 0; x < size; ++x) {
				const size_t offset = (size_t(y) * size + x) * 4;
				source.Pixels[offset] = static_cast<uint8_t>(x * 10);
				source.Pixels[offset + 1] = static_cast<uint8_t>(x * 20);
				source.Pixels[offset + 2] = static_cast<uint8_t>(x * 30);
				source.Pixels[offset + 3] = 255;
			}
		return source;
	}
	Image EmptyLike(const Image &source) {
		return {source.Width, source.Height, std::vector<uint8_t>(source.Pixels.size()), 0};
	}
	size_t At(const Image &image, uint32_t x, uint32_t y) {
		return (size_t(y) * image.Width + x) * 4;
	}
}

TEST_CASE("Mirror reflects one side and emits an exact side mask", "[imagegraph]") {
	Image source{3, 3, std::vector<uint8_t>(36), 0};
	for (uint32_t y = 0; y < 3; ++y)
		for (uint32_t x = 0; x < 3; ++x) {
			const size_t offset = At(source, x, y);
			source.Pixels[offset] = static_cast<uint8_t>(10 + y * 10);
			source.Pixels[offset + 3] = 255;
		}
	Image colored = EmptyLike(source), mask = EmptyLike(source);
	MirrorControl control;
	control.PositionX = control.PositionY = 1.5;
	REQUIRE(RenderMirror(source, colored, mask, control) == SpatialWarpStatus::Ok);
	CHECK(colored.Pixels[At(colored, 1, 0)] == 30);
	CHECK(colored.Pixels[At(colored, 1, 2)] == 30);
	CHECK(mask.Pixels[At(mask, 1, 0)] == 255);
	CHECK(mask.Pixels[At(mask, 1, 2)] == 0);
	CHECK(mask.Pixels[At(mask, 1, 0) + 3] == 255);
}

TEST_CASE("Barrel distance modes move a grid sample by pinned radius power", "[imagegraph]") {
	const Image source = Grid(5);
	Image output = EmptyLike(source);
	BarrelControl control;
	control.CenterX = control.CenterY = 2.5;
	control.Intensity = 2.0;
	REQUIRE(RenderBarrel(source, output, control) == SpatialWarpStatus::Ok);
	CHECK(output.Pixels[At(output, 4, 2)] == 30);
	CHECK(output.Pixels[At(output, 4, 4)] == 30);
	control.DistanceMethod = 1;
	REQUIRE(RenderBarrel(source, output, control) == SpatialWarpStatus::Ok);
	CHECK(output.Pixels[At(output, 4, 4)] == 40);
	control.DistanceMethod = 3;
	REQUIRE(RenderBarrel(source, output, control) == SpatialWarpStatus::Ok);
	CHECK(output.Pixels[At(output, 4, 2)] == 20);
}

TEST_CASE("Chromatic Scale offsets red and blue while accumulating source alpha", "[imagegraph]") {
	const Image source = Grid(5);
	Image output = EmptyLike(source);
	ChromaticControl control;
	control.CenterX = control.CenterY = 2.5;
	REQUIRE(RenderChromaticScale(source, output, control) == SpatialWarpStatus::Ok);
	const size_t edge = At(output, 4, 2);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin() + edge, output.Pixels.begin() + edge + 4) ==
		std::vector<uint8_t>{30, 80, 0, 255}
	);
	const size_t center = At(output, 2, 2);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin() + center, output.Pixels.begin() + center + 4) ==
		std::vector<uint8_t>{20, 40, 60, 255}
	);
}

TEST_CASE("Spherize trims outside its radial domain", "[imagegraph]") {
	const Image source = Grid(5);
	Image output = EmptyLike(source);
	SpherizeControl control;
	control.CenterX = control.CenterY = 2.5;
	REQUIRE(RenderSpherize(source, output, control) == SpatialWarpStatus::Ok);
	CHECK(output.Pixels[At(output, 2, 2)] == 20);
	CHECK(output.Pixels[At(output, 0, 0) + 3] == 0);
}

TEST_CASE("Dilate displaces a neighbor toward its center", "[imagegraph]") {
	const Image source = Grid(5);
	Image output = EmptyLike(source);
	DilateControl control;
	control.CenterX = control.CenterY = 2.5;
	control.Radius = 2.5;
	REQUIRE(RenderDilate(source, output, control) == SpatialWarpStatus::Ok);
	CHECK(source.Pixels[At(source, 3, 2)] == 30);
	CHECK(output.Pixels[At(output, 3, 2)] == 20);
}

TEST_CASE("Spatial warp rejects undefined division and invalid output", "[imagegraph]") {
	const Image source = Grid(5);
	Image output{5, 5, std::vector<uint8_t>(100, 7), 0};
	BarrelControl barrel;
	barrel.ScaleX = 0.0;
	CHECK(RenderBarrel(source, output, barrel) == SpatialWarpStatus::InvalidControl);
	barrel.ScaleX = 1e-308;
	barrel.Intensity = 1e308;
	CHECK(RenderBarrel(source, output, barrel) == SpatialWarpStatus::UndefinedDivision);
	std::fill(output.Pixels.begin(), output.Pixels.end(), 7);
	SpherizeControl spherize;
	spherize.Radius = 0.0;
	CHECK(RenderSpherize(source, output, spherize) == SpatialWarpStatus::InvalidControl);
	DilateControl dilate;
	dilate.Radius = 0.0;
	CHECK(RenderDilate(source, output, dilate) == SpatialWarpStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>(100, 7));
	output.Pixels.pop_back();
	CHECK(RenderDilate(source, output, DilateControl{}) == SpatialWarpStatus::InvalidImage);
}

TEST_CASE("Spatial warp graph routes five static nodes and Mirror Mask", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{5}}, {"height", int64_t{5}}, {"colour", Colour{10, 20, 30, 255}}}},
		{"mirror", "image.mirror", "", {}, {}},
		{"barrel", "image.barrel_distort", "", {}, {{"intensity", 1.0}}},
		{"chromatic", "image.chromatic_aberration", "", {}, {{"strength", 0.0}}},
		{"spherize", "image.spherize", "", {}, {}},
		{"dilate", "image.dilate", "", {}, {{"radius", 2.5}}}
	};
	for (size_t index = 1; index < document.Nodes.size(); ++index)
		document.Links.push_back({"source", "image", document.Nodes[index].Id, "image"});
	document.Outputs = {
		{"mirror", "mirror", "image"},
		{"mirror_mask", "mirror", "mirror_mask"},
		{"barrel", "barrel", "image"},
		{"chromatic", "chromatic", "image"},
		{"spherize", "spherize", "image"},
		{"dilate", "dilate", "image"}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "mirror", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[At(output, 2, 0)] == 10);
	REQUIRE(Evaluate(document, plan, "mirror_mask", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[At(output, 2, 0)] == 255);
	CHECK(output.Pixels[At(output, 2, 4)] == 0);
	REQUIRE(Evaluate(document, plan, "barrel", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[At(output, 2, 2)] == 10);
	REQUIRE(Evaluate(document, plan, "chromatic", output, diagnostic) == Status::Ok);
	CHECK(
		std::vector<uint8_t>(
			output.Pixels.begin() + At(output, 2, 2), output.Pixels.begin() + At(output, 2, 2) + 4
		) == std::vector<uint8_t>{10, 20, 30, 255}
	);
	REQUIRE(Evaluate(document, plan, "spherize", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[At(output, 2, 2)] == 10);
	CHECK(output.Pixels[At(output, 0, 0) + 3] == 0);
	REQUIRE(Evaluate(document, plan, "dilate", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[At(output, 2, 2)] == 10);
	document.Nodes[3].Values.push_back({"type", int64_t{1}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "chromatic", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "chromatic");
}

TEST_CASE("Mirror graph refuses two retained outputs above the evaluation byte budget", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{3500}}, {"height", int64_t{3500}}, {"colour", Colour{1, 2, 3, 4}}}},
		{"mirror", "image.mirror", "", {}, {}}
	};
	document.Links = {{"source", "image", "mirror", "image"}};
	document.Outputs = {{"out", "mirror", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "mirror");
	CHECK(output.Pixels.empty());
}

TEST_CASE("Spatial warp scalar controls cannot be linked as image ports", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{1, 2, 3, 255}}}},
		{"barrel", "image.barrel_distort", "", {}, {}}
	};
	document.Links = {{"source", "image", "barrel", "image"}, {"source", "image", "barrel", "intensity"}};
	document.Outputs = {{"out", "barrel", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::UnknownPort);
	CHECK(diagnostic.NodeId == "barrel");
	CHECK(diagnostic.Port == "intensity");
}
