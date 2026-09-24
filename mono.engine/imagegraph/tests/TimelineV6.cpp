#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.timeline.v6")

namespace {
	using namespace engine::imagegraph;

	Document BrightnessGraph() {
		Document document;
		document.FormatVersion = 6;
		document.Nodes = {
			{"solid",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{64, 64, 64, 255}}}},
			{"adjust", "image.color_adjust", "", {}, {{"brightness", 0.0}}},
		};
		document.Links.push_back({"solid", "image", "adjust", "image"});
		document.Outputs.push_back({"out", "adjust", "image"});
		document.Keyframes = {
			{"adjust", "brightness", 0, 0.0, "linear", std::nullopt},
			{"adjust", "brightness", 3, 0.0, "linear", std::nullopt},
		};
		document.Keyframes.front().SineDriver = KeyframeSineDriver{1.0, 0.25, 0.0, 0.0};
		document.Timeline = TimelineSettings{4, 0, 3, "loop", 30.0};
		document.Tracks = {{"adjust", "brightness", "hold", -1}};
		return document;
	}

	Document NumberGraph() {
		Document document;
		document.FormatVersion = 6;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
		document.Outputs = {{"out", "number", "number"}};
		document.Keyframes = {{"number", "value", 0, 0.0, "linear", std::nullopt}};
		document.Timeline = TimelineSettings{4, 0, 3, "loop", 30.0};
		return document;
	}
}

TEST_CASE("sine keyframe metadata round trips and drives the four-frame image fixture", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document source = BrightnessGraph();
	const std::string text = Write(source);
	CHECK(text.find("key_driver \"adjust\" \"brightness\" 0 \"sine\" 1 0.25 0 0\n") != std::string::npos);

	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == source);

	Plan plan;
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(parsed, plan, "out", EvaluationRequest{0, 0, 0.0}, image, diagnostic) == Status::Ok);
	CHECK((image.Pixels == std::vector<uint8_t>{64, 64, 64, 255}));
	REQUIRE(Evaluate(parsed, plan, "out", EvaluationRequest{1, 0, 0.0}, image, diagnostic) == Status::Ok);
	CHECK((image.Pixels == std::vector<uint8_t>{128, 128, 128, 255}));
	REQUIRE(Evaluate(parsed, plan, "out", EvaluationRequest{3, 0, 0.0}, image, diagnostic) == Status::Ok);
	CHECK((image.Pixels == std::vector<uint8_t>{64, 64, 64, 255}));
}

TEST_CASE("default sine driver parameters use normalized timeline time", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document = NumberGraph();
	document.Keyframes.front().SineDriver = KeyframeSineDriver{};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);

	EvaluatedValue value;
	REQUIRE(
		EvaluateValue(document, plan, "out", EvaluationRequest{0, 0, 0.25}, value, diagnostic) == Status::Ok
	);
	CHECK(std::get<double>(value.Data) == Catch::Approx(1.0));
}

TEST_CASE("legacy graph versions migrate to v6 without creating sine drivers", "[imagegraph]") {
	using namespace engine::imagegraph;
	Diagnostic diagnostic;
	for (uint32_t version = 1; version <= 5; version++) {
		Document document;
		document.FormatVersion = version;
		document.Keyframes = {{"number", "value", 0, 2.0, "linear", std::nullopt}};
		if (version >= 4) document.Timeline = TimelineSettings{4, 0, 3, "loop", 30.0};
		REQUIRE(Migrate(document, diagnostic) == Status::Ok);
		CHECK(document.FormatVersion == 6);
		CHECK_FALSE(document.Keyframes.front().SineDriver.has_value());
		if (document.Timeline) CHECK(document.Timeline->FramesPerSecond == 30.0);
	}
}

TEST_CASE("sine keyframe driver rejects unsupported values and versions", "[imagegraph]") {
	using namespace engine::imagegraph;
	Diagnostic diagnostic;
	Plan plan;
	Document document = NumberGraph();
	document.Keyframes.front().SineDriver = KeyframeSineDriver{1.0, 1.0, 0.0, 1.01};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "value");

	document.Keyframes.front().SineDriver =
		KeyframeSineDriver{1.0, std::numeric_limits<double>::infinity(), 0.0, 0.0};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);

	document.FormatVersion = 5;
	document.Keyframes.front().SineDriver = KeyframeSineDriver{};
	CHECK(Compile(document, plan, diagnostic) == Status::UnsupportedVersion);
}
