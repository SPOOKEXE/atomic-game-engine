#include "../src/PixelOpsChecker.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_checker")

using engine::imagegraph::Image;
using engine::imagegraph::detail::CheckerControls;
using engine::imagegraph::detail::CheckerStatus;
using engine::imagegraph::detail::RenderChecker;

TEST_CASE("Checker solid source equations produce exact two-color pixels", "[imagegraph]") {
	Image output{4, 4, std::vector<uint8_t>(64), 0};
	CheckerControls control;
	control.First = {10, 20, 30, 40};
	control.Second = {50, 60, 70, 80};
	REQUIRE(RenderChecker(output, control) == CheckerStatus::Ok);
	const auto at = [&output](uint32_t x, uint32_t y) {
		return output.Pixels[(size_t(y) * output.Width + x) * 4];
	};
	CHECK(at(0, 0) == 50);
	CHECK(at(1, 1) == 50);
	CHECK(at(2, 0) == 10);
	CHECK(at(0, 2) == 10);
	CHECK(at(3, 3) == 50);
	CHECK(
		std::vector<uint8_t>{output.Pixels[0], output.Pixels[1], output.Pixels[2], output.Pixels[3]} ==
		std::vector<uint8_t>{50, 60, 70, 80}
	);
	const auto previous = output.Pixels;
	REQUIRE(RenderChecker(output, control) == CheckerStatus::Ok);
	CHECK(output.Pixels == previous);
}

TEST_CASE("Checker diagonal solid branch follows the source pixel lattice", "[imagegraph]") {
	Image output{4, 4, std::vector<uint8_t>(64), 0};
	CheckerControls control;
	control.First = {1, 0, 0, 255};
	control.Second = {2, 0, 0, 255};
	control.Diagonal = true;
	REQUIRE(RenderChecker(output, control) == CheckerStatus::Ok);
	for (uint32_t y = 0; y < 4; ++y)
		for (uint32_t x = 0; x < 4; ++x)
			CHECK(output.Pixels[(size_t(y) * 4 + x) * 4] == (x % 2 == 0 ? 1 : 2));
}

TEST_CASE("Checker validates dimension and undefined diagonal period before writing", "[imagegraph]") {
	Image output{2, 2, std::vector<uint8_t>(16, 7), 0};
	CheckerControls control;
	control.Size = 0.0;
	CHECK(RenderChecker(output, control) == CheckerStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>(16, 7));
	control.Size = 0.1;
	control.Diagonal = true;
	CHECK(RenderChecker(output, control) == CheckerStatus::UndefinedRange);
	CHECK(output.Pixels == std::vector<uint8_t>(16, 7));
	output.Pixels.pop_back();
	CHECK(RenderChecker(output, control) == CheckerStatus::InvalidImage);
}

TEST_CASE("Checker graph evaluates solid controls and rejects unsupported render type", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"checker",
		 "image.checker",
		 "",
		 {},
		 {{"width", int64_t{4}},
		  {"height", int64_t{4}},
		  {"size", 0.5},
		  {"diagonal", true},
		  {"color_1", Colour{1, 0, 0, 255}},
		  {"color_2", Colour{2, 0, 0, 255}}}}
	};
	document.Outputs = {{"out", "checker", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[0] == 1);
	CHECK(output.Pixels[4] == 2);
	CHECK(output.Pixels[8] == 1);
	document.Nodes.front().Values.push_back({"type", int64_t{1}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "checker");
}
