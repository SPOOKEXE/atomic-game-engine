#include "../src/PixelOpsBasicFilters.hpp"

#include "../src/PixelOpsBlend.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_basic_filters")

using engine::imagegraph::Image;
using engine::imagegraph::detail::BasicFilterStatus;
using engine::imagegraph::detail::NonPalettePosterize;
using engine::imagegraph::detail::OffsetImage;
using engine::imagegraph::detail::PosterizeWithoutPalette;
using engine::imagegraph::detail::SimpleThreshold;
using engine::imagegraph::detail::ThresholdImage;

namespace {
	engine::imagegraph::Document FilterGraph(std::string type, engine::imagegraph::Colour colour) {
		engine::imagegraph::Document graph;
		graph.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", colour}}},
			{"filter", std::move(type), "", {}, {}},
		};
		graph.Links = {{"source", "image", "filter", "image"}};
		graph.Outputs = {{"out", "filter", "image"}};
		return graph;
	}
}

TEST_CASE("Basic filter schemas expose source-backed typed controls", "[imagegraph]") {
	const auto *offset = engine::imagegraph::FindSchema("image.offset");
	const auto *threshold = engine::imagegraph::FindSchema("image.threshold");
	const auto *posterize = engine::imagegraph::FindSchema("image.posterize");
	REQUIRE(offset != nullptr);
	REQUIRE(threshold != nullptr);
	REQUIRE(posterize != nullptr);
	CHECK(offset->Properties.size() == 6);
	CHECK(threshold->Properties.size() == 16);
	CHECK(posterize->Properties.size() == 11);
}

TEST_CASE("Basic filters compile and evaluate source-visible branches", "[imagegraph]") {
	using namespace engine::imagegraph;
	Plan plan;
	Diagnostic diagnostic;
	Image image;
	Document offset = FilterGraph("image.offset", {64, 128, 192, 255});
	offset.Nodes[1].Values = {{"x_offset", 0.5}, {"angle", 45.0}};
	REQUIRE(Compile(offset, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(offset, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{64, 128, 192, 255});
	Document threshold = FilterGraph("image.threshold", {255, 0, 0, 128});
	threshold.Nodes[1].Values = {{"brightness", true}};
	REQUIRE(Compile(threshold, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(threshold, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 0, 0, 128});
	Document posterize = FilterGraph("image.posterize", {128, 128, 128, 20});
	posterize.Nodes[1].Values = {{"use_palette", false}};
	REQUIRE(Compile(posterize, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(posterize, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{170, 170, 170, 255});
}

TEST_CASE("Basic filters reject invalid branches with named diagnostics", "[imagegraph]") {
	using namespace engine::imagegraph;
	Plan plan;
	Diagnostic diagnostic;
	Image image;
	Document posterize = FilterGraph("image.posterize", {128, 128, 128, 255});
	CHECK(Compile(posterize, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "filter");
	CHECK(diagnostic.Port == "palette");
	posterize.Nodes[1].Values = {{"use_palette", false}, {"steps", int64_t{1}}};
	CHECK(Compile(posterize, plan, diagnostic) == Status::InvalidValue);
	Document threshold = FilterGraph("image.threshold", {128, 128, 128, 255});
	threshold.Nodes[1].Values = {{"algorithm", int64_t{1}}};
	REQUIRE(Compile(threshold, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(threshold, plan, "out", image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "algorithm");
}

TEST_CASE("Palette posterize maps RGB to the first nearest colour and retains alpha", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document posterize = FilterGraph("image.posterize", {12, 0, 0, 71});
	posterize.Nodes[1].Values = {
		{"use_palette", true},
		{"posterize_alpha", false},
		{"palette",
		 ArrayValue{
			 ValueType::Colour, {ElementValue{Colour{10, 0, 0, 1}}, ElementValue{Colour{14, 0, 0, 2}}}
		 }}
	};
	Plan plan;
	Diagnostic diagnostic;
	Image image;
	REQUIRE(Compile(posterize, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(posterize, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 0, 0, 71});

	const std::string encoded = Write(posterize);
	Document decoded;
	REQUIRE(Read(encoded, decoded, diagnostic) == Status::Ok);
	CHECK(decoded == posterize);
	REQUIRE(Compile(decoded, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(decoded, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 0, 0, 71});
}

TEST_CASE("Palette posterize alpha mode uses premultiplied RGB and selected palette alpha", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document posterize = FilterGraph("image.posterize", {200, 0, 0, 128});
	posterize.Nodes[1].Values = {
		{"use_palette", true},
		{"posterize_alpha", true},
		{"palette",
		 ArrayValue{
			 ValueType::Colour, {ElementValue{Colour{100, 0, 0, 55}}, ElementValue{Colour{200, 0, 0, 255}}}
		 }}
	};
	Plan plan;
	Diagnostic diagnostic;
	Image image;
	REQUIRE(Compile(posterize, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(posterize, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{100, 0, 0, 55});
}

TEST_CASE("Palette posterize rejects untyped, empty, and over-limit palettes", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document posterize = FilterGraph("image.posterize", {1, 2, 3, 4});
	Plan plan;
	Diagnostic diagnostic;
	auto setPalette = [&](ArrayValue palette) {
		posterize.Nodes[1].Values = {{"use_palette", true}, {"palette", std::move(palette)}};
	};
	setPalette({ValueType::Integer, {ElementValue{int64_t{1}}}});
	CHECK(Compile(posterize, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "filter");
	CHECK(diagnostic.Port == "palette");
	setPalette({ValueType::Colour, {}});
	CHECK(Compile(posterize, plan, diagnostic) == Status::InvalidValue);
	setPalette(
		{ValueType::Colour,
		 std::vector<ElementValue>(Limits::MaximumPaletteEntries + 1, ElementValue{Colour{}})}
	);
	CHECK(Compile(posterize, plan, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Port == "palette");
}

TEST_CASE("Generic mask inversion follows feathering while Blend converts first", "[imagegraph]") {
	const Image mask{3, 1, {119, 23, 111, 98, 146, 158, 168, 249, 18, 3, 46, 182}, 0};
	const Image original{3, 1, {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255}, 0};
	Image generic{3, 1, std::vector<uint8_t>(12, 255), 0};
	Image blend = generic;
	const Image feathered = engine::imagegraph::detail::FeatherMask(mask, 2.0);
	engine::imagegraph::detail::ApplyMaskMix(original, generic, &feathered, 1.0, true);
	CHECK(generic.Pixels == std::vector<uint8_t>{22, 22, 22, 255, 43, 43, 43, 255, 38, 38, 38, 255});
	const Image modified = engine::imagegraph::detail::ModifyBlendMask(mask, true, false, 2.0);
	engine::imagegraph::detail::ApplyMaskMix(original, blend, &modified, 1.0);
	CHECK(blend.Pixels == std::vector<uint8_t>{22, 22, 22, 255, 44, 44, 44, 255, 38, 38, 38, 255});
}

TEST_CASE("Offset wraps source pixels by normalized displacement", "[imagegraph]") {
	const Image source{4, 1, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255}, 0};
	Image output{4, 1, std::vector<uint8_t>(16), 0};
	REQUIRE(OffsetImage(source, output, 0.25, 0.0, 0.0) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{40, 0, 0, 255, 10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255});
	REQUIRE(OffsetImage(source, output, 0.0, 0.0, 45.0) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
}

TEST_CASE("Offset angle rotates displacement before wrapped sampling", "[imagegraph]") {
	const Image source{1, 4, {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255}, 0};
	Image output{1, 4, std::vector<uint8_t>(16), 0};
	REQUIRE(OffsetImage(source, output, 0.25, 0.0, 90.0) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{40, 0, 0, 255, 10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255});
	CHECK(
		OffsetImage(source, output, 0.0, 0.0, std::numeric_limits<double>::infinity()) ==
		BasicFilterStatus::InvalidControl
	);
}

TEST_CASE("Simple Threshold applies luminance, alpha, and source order", "[imagegraph]") {
	const Image source{2, 1, {255, 0, 0, 127, 0, 255, 0, 128}, 0};
	Image output{2, 1, std::vector<uint8_t>(8), 0};
	SimpleThreshold control;
	control.Brightness = true;
	REQUIRE(ThresholdImage(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 127, 255, 255, 255, 128});
	control.ApplyToAlpha = 1;
	control.Alpha = true;
	REQUIRE(ThresholdImage(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 0, 0, 0, 255, 0, 255});
	control.ApplyToAlpha = 2;
	control.BrightnessMultiply = true;
	control.Alpha = false;
	REQUIRE(ThresholdImage(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 0, 0, 255, 0, 128});
}

TEST_CASE("Simple Threshold smoothstep and invert use source comparisons", "[imagegraph]") {
	const Image source{1, 1, {0, 0, 0, 255}, 0};
	Image output{1, 1, std::vector<uint8_t>(4), 0};
	SimpleThreshold control;
	control.Brightness = true;
	control.BrightnessThreshold = 0.0;
	control.BrightnessSmoothness = 0.5;
	REQUIRE(ThresholdImage(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	control.BrightnessInvert = true;
	REQUIRE(ThresholdImage(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
}

TEST_CASE("Nonpalette Posterize follows source step and gamma branch", "[imagegraph]") {
	const Image source{3, 1, {64, 64, 64, 10, 128, 128, 128, 20, 255, 255, 255, 30}, 0};
	Image output{3, 1, std::vector<uint8_t>(12), 0};
	NonPalettePosterize control;
	REQUIRE(PosterizeWithoutPalette(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{85, 85, 85, 255, 170, 170, 170, 255, 255, 255, 255, 255});
	control.PosterizeAlpha = false;
	control.Gamma = 2.0;
	REQUIRE(PosterizeWithoutPalette(source, output, control) == BasicFilterStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 10, 147, 147, 147, 20, 255, 255, 255, 30});
}

TEST_CASE("Nonpalette Posterize reports zero local channel range", "[imagegraph]") {
	const Image source{1, 1, {50, 50, 50, 128}, 0};
	Image output{1, 1, {9, 9, 9, 9}, 0};
	NonPalettePosterize control;
	control.GlobalRange = false;
	CHECK(PosterizeWithoutPalette(source, output, control) == BasicFilterStatus::UndefinedDivision);
	CHECK(output.Pixels == std::vector<uint8_t>{9, 9, 9, 9});
	control.Steps = 1;
	CHECK(PosterizeWithoutPalette(source, output, control) == BasicFilterStatus::InvalidControl);
}
