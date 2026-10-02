#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_value_nodes")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE("Boolean preserves its typed default and explicit value", "[imagegraph][source_value]") {
	for (const bool value : {false, true}) {
		const auto result = RunNode("pc.boolean", {}, {{"value", value}});
		INFO(result.Message);
		REQUIRE(result.Ok);
		REQUIRE(result.OutputValue("boolean"));
		CHECK(std::get<bool>(*result.OutputValue("boolean")) == value);
	}
	const auto defaults = RunNode("pc.boolean", {});
	REQUIRE(defaults.Ok);
	CHECK_FALSE(std::get<bool>(*defaults.OutputValue("boolean")));
}

TEST_CASE("RGB source constructor clamps and rounds halves to even", "[imagegraph][source_value]") {
	const auto defaults = RunNode("pc.color_rgb", {});
	INFO(defaults.Message);
	REQUIRE(defaults.Ok);
	CHECK(std::get<Colour>(*defaults.OutputValue("color")) == Colour{255, 255, 255, 255});
	const auto result = RunNode(
		"pc.color_rgb",
		{},
		{{"normalized", false}, {"red", 2.5}, {"green", 3.5}, {"blue", -4.0}, {"alpha", 999.0}}
	);
	REQUIRE(result.Ok);
	CHECK(std::get<Colour>(*result.OutputValue("color")) == Colour{2, 4, 0, 255});
	CHECK_FALSE(RunNode("pc.color_rgb", {}, {{"red", std::numeric_limits<double>::infinity()}}).Ok);
}

TEST_CASE("HSV preserves the source nonnormalized clamp", "[imagegraph][source_value]") {
	const auto normalized = RunNode("pc.color_hsv", {}, {{"hue", 0.0}, {"saturation", 1.0}, {"value", 1.0}});
	INFO(normalized.Message);
	REQUIRE(normalized.Ok);
	CHECK(std::get<Colour>(*normalized.OutputValue("color")) == Colour{255, 0, 0, 255});
	const auto raw = RunNode(
		"pc.color_hsv",
		{},
		{{"normalized", false}, {"hue", 100.0}, {"saturation", 100.0}, {"value", 100.0}, {"alpha", 100.0}}
	);
	REQUIRE(raw.Ok);
	CHECK(std::get<Colour>(*raw.OutputValue("color")) == Colour{1, 1, 1, 1});
}

TEST_CASE("Color Data publishes all components and weighted brightness", "[imagegraph][source_value]") {
	const auto normalized = RunNode("pc.color_data", {}, {{"color", Colour{255, 0, 0, 128}}});
	INFO(normalized.Message);
	REQUIRE(normalized.Ok);
	CHECK(normalized.Values.size() == 8);
	CHECK(std::get<double>(*normalized.OutputValue("red")) == 1.0);
	CHECK(std::get<double>(*normalized.OutputValue("hue")) == 0.0);
	CHECK(std::get<double>(*normalized.OutputValue("saturation")) == 1.0);
	CHECK(std::get<double>(*normalized.OutputValue("brightness")) == Catch::Approx(std::sqrt(.241)));
	CHECK(std::get<double>(*normalized.OutputValue("alpha")) == Catch::Approx(128.0 / 255.0));
	const auto raw = RunNode("pc.color_data", {}, {{"normalize", false}, {"color", Colour{0, 255, 0, 128}}});
	REQUIRE(raw.Ok);
	CHECK(std::get<double>(*raw.OutputValue("green")) == 255.0);
	CHECK(std::get<double>(*raw.OutputValue("alpha")) == 128.0);
}

TEST_CASE("Linked colour construction retains animated source controls", "[imagegraph][source_value]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"rgb", "pc.color_rgb", "", {}, {}}};
	document.Outputs = {{"colour", "rgb", "color"}};
	document.Keyframes = {{"rgb", "red", 0, 0.0, "linear"}, {"rgb", "red", 2, 1.0}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(
		EvaluateValue(document, plan, "colour", EvaluationRequest{.Tick = 1}, value, diagnostic) == Status::Ok
	);
	CHECK(std::get<Colour>(value.Data) == Colour{128, 255, 255, 255});
}

TEST_CASE("Colour channel nodes expose normalized RGB and HSV without alpha", "[imagegraph][source_value]") {
	const auto rgb = RunNode("pc.color_to_rgb", {}, {{"color", Colour{17, 128, 255, 0}}});
	INFO(rgb.Message);
	REQUIRE(rgb.Ok);
	REQUIRE(rgb.Values.size() == 3);
	CHECK(std::get<double>(*rgb.OutputValue("red")) == Catch::Approx(17.0 / 255));
	CHECK(std::get<double>(*rgb.OutputValue("green")) == Catch::Approx(128.0 / 255));
	CHECK(std::get<double>(*rgb.OutputValue("blue")) == 1.0);
	const auto hsv = RunNode("pc.color_to_hsv", {}, {{"color", Colour{0, 255, 0, 42}}});
	INFO(hsv.Message);
	REQUIRE(hsv.Ok);
	REQUIRE(hsv.Values.size() == 3);
	CHECK(std::get<double>(*hsv.OutputValue("hue")) == Catch::Approx(1.0 / 3));
	CHECK(std::get<double>(*hsv.OutputValue("saturation")) == 1.0);
	CHECK(std::get<double>(*hsv.OutputValue("value")) == 1.0);
	const auto grey = RunNode("pc.color_to_hsv", {}, {{"color", Colour{128, 128, 128, 255}}});
	REQUIRE(grey.Ok);
	CHECK(std::get<double>(*grey.OutputValue("hue")) == 0.0);
	CHECK(std::get<double>(*grey.OutputValue("saturation")) == 0.0);
}

TEST_CASE("OKLCH exposes source raw RGB and distinct gamut clipping paths", "[imagegraph][source_value]") {
	const auto defaults = RunNode("pc.color_oklch", {});
	INFO(defaults.Message);
	REQUIRE(defaults.Ok);
	CHECK(std::get<Colour>(*defaults.OutputValue("color")) == Colour{180, 6, 95, 255});
	const auto raw = std::get<Vector3>(*defaults.OutputValue("raw_rgb"));
	CHECK(raw.X == Catch::Approx(.7049172512020752));
	CHECK(raw.Y == Catch::Approx(.023513775066131268));
	CHECK(raw.Z == Catch::Approx(.37073455290498397));
	for (int64_t mode = 0; mode < 4; ++mode) {
		const auto clipped = RunNode(
			"pc.color_oklch", {}, {{"chroma", .6}, {"alpha", .5}, {"gamut_clipping", EnumValue{mode}}}
		);
		INFO(mode);
		INFO(clipped.Message);
		REQUIRE(clipped.Ok);
		const auto colour = std::get<Colour>(*clipped.OutputValue("color"));
		CHECK(colour.Alpha == 128);
		if (mode == 0) CHECK(colour == Colour{181, 0, 94, 128});
		// grug keep source grey clip paths' negative fractional powers and NaN-to-zero result.
		if (mode == 1 || mode == 2) CHECK(colour == Colour{0, 0, 0, 128});
	}
	const auto neutral = RunNode("pc.color_oklch", {}, {{"chroma", 0.0}});
	REQUIRE(neutral.Ok);
	CHECK(std::get<Colour>(*neutral.OutputValue("color")) == Colour{99, 99, 99, 255});
}

TEST_CASE("Colour blend uses source luminosity branches and byte rounding", "[imagegraph][source_value]") {
	const auto dark = RunNode(
		"pc.color_math",
		{},
		{{"blend_mode", EnumValue{13}},
		 {"color_0", Colour{64, 128, 192, 1}},
		 {"color_1", Colour{255, 0, 0, 2}}}
	);
	INFO(dark.Message);
	REQUIRE(dark.Ok);
	CHECK(std::get<Colour>(*dark.OutputValue("result")) == Colour{128, 0, 0, 255});
	const auto light = RunNode(
		"pc.color_math",
		{},
		{{"blend_mode", EnumValue{13}},
		 {"color_0", Colour{64, 128, 192, 1}},
		 {"color_1", Colour{0, 255, 0, 2}}}
	);
	REQUIRE(light.Ok);
	CHECK(std::get<Colour>(*light.OutputValue("result")) == Colour{0, 255, 129, 255});
	const auto halfway = RunNode(
		"pc.color_math",
		{},
		{{"blend_mode", EnumValue{0}},
		 {"color_0", Colour{0, 2, 4, 0}},
		 {"color_1", Colour{255, 3, 5, 0}},
		 {"intensity", .5}}
	);
	REQUIRE(halfway.Ok);
	CHECK(std::get<Colour>(*halfway.OutputValue("result")) == Colour{128, 2, 4, 255});
}

TEST_CASE(
	"Colour blend retains source unhandled equal mode and guarded division", "[imagegraph][source_value]"
) {
	const auto equal = RunNode(
		"pc.color_math",
		{},
		{{"blend_mode", EnumValue{29}},
		 {"color_0", Colour{5, 7, 9, 0}},
		 {"color_1", Colour{5, 7, 9, 0}},
		 {"intensity", .5}}
	);
	REQUIRE(equal.Ok);
	CHECK(std::get<Colour>(*equal.OutputValue("result")) == Colour{2, 4, 4, 255});
	const auto divide = RunNode(
		"pc.color_math",
		{},
		{{"blend_mode", EnumValue{23}},
		 {"color_0", Colour{0, 64, 128, 0}},
		 {"color_1", Colour{0, 128, 255, 0}}}
	);
	REQUIRE(divide.Ok);
	CHECK(std::get<Colour>(*divide.OutputValue("result")) == Colour{255, 128, 128, 255});
	for (const int64_t mode :
		 {0, 1, 3, 4, 5, 6, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 25, 26, 27, 29, 30}) {
		const auto result = RunNode(
			"pc.color_math",
			{},
			{{"blend_mode", EnumValue{mode}},
			 {"color_0", Colour{40, 80, 120, 20}},
			 {"color_1", Colour{80, 120, 160, 30}}}
		);
		INFO(mode);
		INFO(result.Message);
		REQUIRE(result.Ok);
		CHECK(std::get<Colour>(*result.OutputValue("result")).Alpha == 255);
	}
}
