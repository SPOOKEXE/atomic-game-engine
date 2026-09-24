#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

TEST_SUITE_ID("engine.imagegraph.timeline_v5")

namespace graph = engine::imagegraph;

namespace {
	graph::Document NumberTrack(uint32_t version) {
		graph::Document document;
		document.FormatVersion = version;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
		document.Outputs = {{"out", "number", "number"}};
		return document;
	}

	graph::Status Sample(
		const graph::Document &document,
		graph::EvaluationRequest request,
		double &value,
		graph::Diagnostic &diagnostic
	) {
		graph::Plan plan;
		const graph::Status compiled = graph::Compile(document, plan, diagnostic);
		if (compiled != graph::Status::Ok) return compiled;
		graph::EvaluatedValue result;
		const graph::Status evaluated =
			graph::EvaluateValue(document, plan, "out", request, result, diagnostic);
		if (evaluated == graph::Status::Ok) value = std::get<double>(result.Data);
		return evaluated;
	}

	graph::Keyframe SourceKey(uint64_t tick, double value) {
		return {"number", "value", tick, value, "source", graph::KeyframeEase{}};
	}
}

TEST_CASE(
	"v1 through v4 migrate to v6 without changing integer samples or adding a timeline", "[imagegraph]"
) {
	for (uint32_t version : {1u, 2u, 3u, 4u}) {
		graph::Document document = NumberTrack(version);
		document.Keyframes = {
			{"number", "value", 0, 2.0, "linear"},
			{"number", "value", 10, 8.0, "step"},
		};
		graph::Diagnostic diagnostic;
		double before = 0;
		REQUIRE(Sample(document, {5, 0}, before, diagnostic) == graph::Status::Ok);
		REQUIRE(graph::Migrate(document, diagnostic) == graph::Status::Ok);
		CHECK(document.FormatVersion == 6);
		CHECK_FALSE(document.Timeline.has_value());
		double after = 0;
		REQUIRE(Sample(document, {5, 0, 0}, after, diagnostic) == graph::Status::Ok);
		CHECK(after == before);
		graph::Document restored;
		REQUIRE(graph::Read(graph::Write(document), restored, diagnostic) == graph::Status::Ok);
		CHECK(restored == document);
	}
}

TEST_CASE("v5 timeline roundtrips authored FPS and rejects invalid rates", "[imagegraph]") {
	graph::Document document = NumberTrack(5);
	document.Timeline = graph::TimelineSettings{8, 0, 7, "loop", 240.0};
	graph::Plan plan;
	graph::Diagnostic diagnostic;
	REQUIRE(graph::Compile(document, plan, diagnostic) == graph::Status::Ok);
	const std::string saved = graph::Write(document);
	CHECK(saved.find("timeline 8 0 7 \"loop\" 240\n") != std::string::npos);
	graph::Document restored;
	REQUIRE(graph::Read(saved, restored, diagnostic) == graph::Status::Ok);
	CHECK(restored == document);
	for (double invalid :
		 {0.0,
		  -1.0,
		  std::numeric_limits<double>::infinity(),
		  std::numeric_limits<double>::quiet_NaN(),
		  std::numeric_limits<double>::denorm_min()}) {
		document.Timeline->FramesPerSecond = invalid;
		CHECK(graph::Compile(document, plan, diagnostic) == graph::Status::InvalidValue);
		CHECK(diagnostic.Port == "timeline");
	}
	CHECK(
		graph::Read("imagegraph 5\ntimeline 8 0 7 \"loop\" 0\n", restored, diagnostic) ==
		graph::Status::Malformed
	);
	CHECK(
		graph::Read("imagegraph 5\ntimeline 8 0 7 \"loop\"\n", restored, diagnostic) ==
		graph::Status::Malformed
	);
	graph::Document legacy = NumberTrack(4);
	legacy.Timeline = graph::TimelineSettings{8, 0, 7, "loop"};
	REQUIRE(graph::Migrate(legacy, diagnostic) == graph::Status::Ok);
	REQUIRE(legacy.Timeline.has_value());
	CHECK(legacy.Timeline->FramesPerSecond == 30.0);
}

TEST_CASE("fractional key requests interpolate and preserve integer results", "[imagegraph]") {
	graph::Document document = NumberTrack(5);
	document.Keyframes = {SourceKey(2, 0.0), SourceKey(6, 8.0)};
	document.Tracks = {{"number", "value", "hold", -1}};
	graph::Diagnostic diagnostic;
	double value = 0;
	REQUIRE(Sample(document, {3, 0, 0.5}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(3.0));
	REQUIRE(Sample(document, {4, 0, 0}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 4.0);
	document.Keyframes[1].Ease->InType = "cut";
	REQUIRE(Sample(document, {3, 0, 0.5}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 0.0);
	document.Keyframes[1].Ease->InType = "linear";
	document.Tracks.clear();
	document.Keyframes[0].Interpolation = "linear";
	document.Keyframes[0].Ease.reset();
	document.Keyframes[1].Interpolation = "step";
	document.Keyframes[1].Ease.reset();
	REQUIRE(Sample(document, {3, 0, 0.5}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(3.0));
	REQUIRE(Sample(document, {4, 0, 0}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 4.0);
}

TEST_CASE("fractional ping reverses and wrap uses the closing segment", "[imagegraph]") {
	graph::Document document = NumberTrack(5);
	document.Timeline = graph::TimelineSettings{8, 0, 7, "loop"};
	document.Keyframes = {SourceKey(2, 2.0), SourceKey(5, 5.0)};
	document.Tracks = {{"number", "value", "ping", -1}};
	graph::Diagnostic diagnostic;
	double value = 0;
	REQUIRE(Sample(document, {6, 0, 0.5}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(3.5));
	document.Tracks[0].End = "wrap";
	REQUIRE(Sample(document, {6, 0, 0.5}, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(4.1));
}

TEST_CASE("fractional requests interpolate vector and colour values through typed outputs", "[imagegraph]") {
	graph::Document document;
	document.FormatVersion = 5;
	document.Nodes = {
		{"vector", "value.vector_magnitude", "", {}, {{"vector", graph::Vector2{0, 0}}}},
		{"colour",
		 "value.color_data",
		 "",
		 {},
		 {{"colour", graph::Colour{0, 0, 0, 255}}, {"normalized", false}}},
	};
	document.Outputs = {{"length", "vector", "result"}, {"red", "colour", "red"}};
	document.Keyframes = {
		{"vector", "vector", 0, graph::Vector2{0, 0}, "source", graph::KeyframeEase{}},
		{"vector", "vector", 2, graph::Vector2{4, 0}, "source", graph::KeyframeEase{}},
		{"colour", "colour", 0, graph::Colour{0, 0, 0, 255}, "source", graph::KeyframeEase{}},
		{"colour", "colour", 2, graph::Colour{255, 0, 0, 255}, "source", graph::KeyframeEase{}},
	};
	document.Tracks = {{"vector", "vector", "hold", -1}, {"colour", "colour", "hold", -1}};
	graph::Plan plan;
	graph::Diagnostic diagnostic;
	REQUIRE(graph::Compile(document, plan, diagnostic) == graph::Status::Ok);
	graph::EvaluatedValue value;
	REQUIRE(
		graph::EvaluateValue(document, plan, "length", {0, 0, 0.5}, value, diagnostic) == graph::Status::Ok
	);
	CHECK(std::get<double>(value.Data) == Catch::Approx(1.0));
	REQUIRE(graph::EvaluateValue(document, plan, "red", {0, 0, 0.5}, value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<double>(value.Data) == 64.0);
}

TEST_CASE("subframe request rejects invalid values and the final tick boundary", "[imagegraph]") {
	graph::Document document = NumberTrack(5);
	graph::Diagnostic diagnostic;
	double value = 0;
	for (double invalid :
		 {-0.1, 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		CHECK(Sample(document, {0, 0, invalid}, value, diagnostic) == graph::Status::InvalidValue);
		CHECK(diagnostic.Port == "subframe");
	}
	CHECK(
		Sample(document, {graph::Limits::MaximumTick, 0, 0.5}, value, diagnostic) ==
		graph::Status::InvalidValue
	);
	CHECK(diagnostic.Port == "subframe");
	CHECK(Sample(document, {graph::Limits::MaximumTick, 0, 0}, value, diagnostic) == graph::Status::Ok);
}
