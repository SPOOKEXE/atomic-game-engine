#include "../src/PixelOpsBlend.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_blend")

using engine::imagegraph::Colour;
using engine::imagegraph::detail::BlendPixel;
using engine::imagegraph::detail::BlendPixelStatus;

namespace {
	engine::imagegraph::Node BlendSolid(std::string id, Colour colour) {
		return {
			std::move(id),
			"image.solid",
			"",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", colour}}
		};
	}
}

TEST_CASE("Blend graph composes linked surfaces and source mask controls", "[imagegraph]") {
	engine::imagegraph::Document document;
	document.Nodes = {
		BlendSolid("background", {64, 128, 192, 192}),
		BlendSolid("foreground", {200, 100, 50, 128}),
		BlendSolid("mask", {0, 0, 0, 128}),
		{"blend", "image.blend", "", {}, {}},
	};
	document.Links = {
		{"background", "image", "blend", "background"},
		{"foreground", "image", "blend", "foreground"},
	};
	document.Outputs = {{"out", "blend", "image"}};
	engine::imagegraph::Plan plan;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Compile(document, plan, diagnostic) == engine::imagegraph::Status::Ok);
	engine::imagegraph::Image output;
	REQUIRE(
		engine::imagegraph::Evaluate(document, plan, "out", output, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(output.Pixels == std::vector<uint8_t>{142, 112, 111, 224});
	const uint64_t hash = output.Hash;
	REQUIRE(
		engine::imagegraph::Evaluate(document, plan, "out", output, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(output.Hash == hash);
	document.Links.push_back({"mask", "image", "blend", "mask"});
	document.Nodes[3].Values.push_back({"invert_mask", true});
	REQUIRE(engine::imagegraph::Compile(document, plan, diagnostic) == engine::imagegraph::Status::Ok);
	REQUIRE(
		engine::imagegraph::Evaluate(document, plan, "out", output, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(output.Pixels == std::vector<uint8_t>{106, 119, 148, 208});
	document.Nodes[3].Values.push_back({"output_dimension", int64_t{4}});
	CHECK(
		engine::imagegraph::Compile(document, plan, diagnostic) == engine::imagegraph::Status::InvalidValue
	);
	document.Nodes[3].Values.push_back({"constant_dimension", engine::imagegraph::Vector2{2.0, 1.0}});
	document.Nodes[3].Values.push_back({"fill_mode", int64_t{1}});
	REQUIRE(engine::imagegraph::Compile(document, plan, diagnostic) == engine::imagegraph::Status::Ok);
	REQUIRE(
		engine::imagegraph::Evaluate(document, plan, "out", output, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(output.Width == 2);
	CHECK(output.Pixels == std::vector<uint8_t>{106, 119, 148, 208, 106, 119, 148, 208});
}

TEST_CASE("observed PXC Blend modes follow their pinned shader pixel equations", "[imagegraph]") {
	const Colour background{64, 128, 192, 192};
	const Colour foreground{200, 100, 50, 128};
	const double maskAmount = engine::imagegraph::detail::BlendMaskAmount({255, 255, 255, 128}, false);
	struct Expected {
		int64_t Mode;
		Colour Pixel;
	};
	constexpr std::array expected{
		Expected{0, {96, 121, 158, 204}},
		Expected{1, {115, 117, 139, 168}},
		Expected{3, {67, 113, 153, 156}},
		Expected{8, {108, 144, 193, 240}},
		Expected{9, {105, 157, 222, 204}},
		Expected{11, {75, 128, 192, 192}},
		Expected{22, {25, 103, 172, 156}},
	};
	for (const Expected &caseValue : expected) {
		Colour output{};
		REQUIRE(
			BlendPixel(background, foreground, caseValue.Mode, 0.75, maskAmount, false, output) ==
			BlendPixelStatus::Ok
		);
		CHECK(output == caseValue.Pixel);
		Colour repeated{};
		REQUIRE(
			BlendPixel(background, foreground, caseValue.Mode, 0.75, maskAmount, false, repeated) ==
			BlendPixelStatus::Ok
		);
		CHECK(repeated == output);
	}
}

TEST_CASE("remaining source Blend modes have exact RGBA8 reference pixels", "[imagegraph]") {
	const Colour background{64, 128, 192, 192};
	const Colour foreground{200, 100, 50, 128};
	const double maskAmount = 128.0 / 255.0;
	struct Expected {
		int64_t Mode;
		Colour Pixel;
	};
	constexpr std::array expected{
		Expected{4, {43, 22, 58, 40}},
		Expected{5, {41, 51, 96, 76}},
		Expected{6, {64, 38, 19, 48}},
		Expected{10, {223, 211, 247, 194}},
		Expected{13, {114, 134, 131, 102}},
		Expected{14, {93, 150, 199, 158}},
		Expected{15, {106, 141, 158, 125}},
		Expected{16, {0, 255, 255, 255}},
		Expected{17, {182, 121, 131, 103}},
		Expected{18, {80, 160, 175, 138}},
		Expected{20, {91, 90, 173, 216}},
		Expected{21, {150, 160, 222, 178}},
		Expected{23, {92, 255, 255, 255}},
		Expected{25, {101, 141, 191, 192}},
		Expected{26, {70, 144, 219, 192}},
		Expected{27, {72, 145, 219, 192}},
		Expected{29, {0, 0, 0, 0}},
		Expected{30, {96, 96, 96, 96}},
	};
	for (const Expected &caseValue : expected) {
		Colour output{};
		REQUIRE(
			BlendPixel(background, foreground, caseValue.Mode, 0.75, maskAmount, false, output) ==
			BlendPixelStatus::Ok
		);
		CHECK(output == caseValue.Pixel);
	}
}

TEST_CASE("Blend mask and preserve-alpha controls affect the exact alpha", "[imagegraph]") {
	const Colour background{64, 128, 192, 192};
	const Colour foreground{200, 100, 50, 128};
	CHECK(engine::imagegraph::detail::BlendMaskAmount({0, 0, 0, 255}, false) == 0.0);
	CHECK(engine::imagegraph::detail::BlendMaskAmount({0, 0, 0, 255}, true) == 1.0);
	Colour output{};
	REQUIRE(BlendPixel(background, foreground, 0, 1.0, 0.0, false, output) == BlendPixelStatus::Ok);
	CHECK(output == background);
	REQUIRE(BlendPixel(background, foreground, 0, 1.0, 1.0, true, output) == BlendPixelStatus::Ok);
	CHECK(output.Alpha == background.Alpha);
	CHECK(output.Red != background.Red);
	REQUIRE(BlendPixel(background, background, 29, 0.5, 1.0, false, output) == BlendPixelStatus::Ok);
	CHECK(output == Colour{128, 128, 128, 128});
	REQUIRE(BlendPixel(background, foreground, 29, 0.5, 1.0, true, output) == BlendPixelStatus::Ok);
	CHECK(output == Colour{0, 0, 0, 192});
	REQUIRE(BlendPixel(background, foreground, 20, 0.5, 1.0, true, output) == BlendPixelStatus::Ok);
	CHECK(output.Alpha == 224);
}

TEST_CASE("Blend reports unsupported modes and undefined transparent division", "[imagegraph]") {
	Colour output{};
	CHECK(BlendPixel({0, 0, 0, 0}, {0, 0, 0, 0}, 0, 1.0, 1.0, false, output) == BlendPixelStatus::Ok);
	CHECK(output == Colour{0, 0, 0, 0});
	for (int64_t mode : {3, 8, 9, 22}) {
		CHECK(
			BlendPixel({0, 0, 0, 0}, {0, 0, 0, 0}, mode, 1.0, 1.0, false, output) ==
			BlendPixelStatus::UndefinedDivision
		);
	}
	CHECK(BlendPixel({}, {}, 2, 1.0, 1.0, false, output) == BlendPixelStatus::UnsupportedMode);
	CHECK(
		BlendPixel({255, 255, 255, 255}, {0, 0, 0, 255}, 10, 1.0, 1.0, false, output) ==
		BlendPixelStatus::UndefinedDivision
	);
	CHECK(
		BlendPixel({0, 0, 0, 255}, {0, 0, 0, 255}, 23, 1.0, 1.0, false, output) ==
		BlendPixelStatus::UndefinedDivision
	);
}

TEST_CASE("Blend canvas places, stretches, tiles and masks the foreground", "[imagegraph]") {
	const engine::imagegraph::Image background{3, 1, {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255}, 0};
	const engine::imagegraph::Image foreground{1, 1, {255, 255, 255, 255}, 0};
	engine::imagegraph::Image output{3, 1, std::vector<uint8_t>(12), 0};
	REQUIRE(
		engine::imagegraph::detail::BlendCanvas(
			background, &foreground, nullptr, output, 0, 1.0, false, 0, {0.5, 0.5}, false
		) == BlendPixelStatus::Ok
	);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255});
	for (int64_t fillMode : {1, 2}) {
		REQUIRE(
			engine::imagegraph::detail::BlendCanvas(
				background, &foreground, nullptr, output, 0, 1.0, false, fillMode, {0.5, 0.5}, false
			) == BlendPixelStatus::Ok
		);
		CHECK(
			output.Pixels == std::vector<uint8_t>{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}
		);
	}
	const engine::imagegraph::Image mask{3, 1, {255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255}, 0};
	REQUIRE(
		engine::imagegraph::detail::BlendCanvas(
			background, &foreground, &mask, output, 0, 1.0, false, 1, {0.5, 0.5}, false
		) == BlendPixelStatus::Ok
	);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255});
}

TEST_CASE("Blend mask modifier inverts RGB before alpha and feather", "[imagegraph]") {
	const engine::imagegraph::Image mask{1, 1, {0, 0, 0, 128}, 0};
	const engine::imagegraph::Image modified =
		engine::imagegraph::detail::ModifyBlendMask(mask, true, false, 0.0);
	CHECK(modified.Pixels == std::vector<uint8_t>{255, 255, 255, 128});
	CHECK(
		engine::imagegraph::detail::BlendMaskAmount(
			engine::imagegraph::detail::ReadBlendColour(modified, 0), false
		) == 128.0 / 255.0
	);
	const engine::imagegraph::Image alphaOnly =
		engine::imagegraph::detail::ModifyBlendMask(mask, true, true, 0.0);
	CHECK(alphaOnly.Pixels == std::vector<uint8_t>{255, 255, 255, 127});
	const engine::imagegraph::Image untouched =
		engine::imagegraph::detail::ModifyBlendMask(mask, false, false, 0.0);
	CHECK(untouched.Pixels == mask.Pixels);
}
