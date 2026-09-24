#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>

TEST_SUITE_ID("engine.imagegraph.timeline_v4")

namespace graph = engine::imagegraph;

namespace {
	graph::Document NumberTrack(uint32_t version) {
		graph::Document document;
		document.FormatVersion = version;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
		document.Outputs = {{"out", "number", "number"}};
		return document;
	}

	graph::Status
	Sample(const graph::Document &document, uint64_t tick, double &value, graph::Diagnostic &diagnostic) {
		graph::Plan plan;
		const graph::Status compiled = graph::Compile(document, plan, diagnostic);
		if (compiled != graph::Status::Ok) return compiled;
		graph::EvaluatedValue result;
		const graph::Status evaluated =
			graph::EvaluateValue(document, plan, "out", {tick, 0}, result, diagnostic);
		if (evaluated == graph::Status::Ok) value = std::get<double>(result.Data);
		return evaluated;
	}

	graph::Keyframe SourceKey(uint64_t tick, double value) {
		return {"number", "value", tick, value, "source", graph::KeyframeEase{}};
	}
}

TEST_CASE("v1 through v3 keyed values migrate to v6 without changing left-interval step", "[imagegraph]") {
	for (uint32_t version : {1u, 2u, 3u}) {
		graph::Document document = NumberTrack(version);
		document.Keyframes = {
			{"number", "value", 0, 2.0, "step"},
			{"number", "value", 10, 8.0, "linear"},
		};
		graph::Diagnostic diagnostic;
		double before = 0;
		REQUIRE(Sample(document, 5, before, diagnostic) == graph::Status::Ok);
		CHECK(before == 2.0);
		REQUIRE(graph::Migrate(document, diagnostic) == graph::Status::Ok);
		CHECK(document.FormatVersion == 6);
		CHECK_FALSE(document.Timeline.has_value());
		CHECK(document.Tracks.empty());
		CHECK_FALSE(document.Keyframes.front().Ease.has_value());
		double after = 0;
		REQUIRE(Sample(document, 5, after, diagnostic) == graph::Status::Ok);
		CHECK(after == before);
		graph::Document restored;
		REQUIRE(graph::Read(graph::Write(document), restored, diagnostic) == graph::Status::Ok);
		CHECK(restored == document);
	}
}

TEST_CASE(
	"v4 source tracks route hold loop ping and wrap without changing the authored keys", "[imagegraph]"
) {
	graph::Document document = NumberTrack(4);
	document.Timeline = graph::TimelineSettings{8, 0, 7, "pingpong"};
	document.Keyframes = {SourceKey(2, 2.0), SourceKey(5, 5.0)};
	document.Tracks = {{"number", "value", "hold", -1}};
	const std::string authored = graph::Write(document);
	graph::Document restored;
	graph::Diagnostic diagnostic;
	REQUIRE(graph::Read(authored, restored, diagnostic) == graph::Status::Ok);
	CHECK(restored == document);
	CHECK(graph::Write(restored) == authored);
	double value = 0;
	REQUIRE(Sample(document, 3, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 3.0);
	REQUIRE(Sample(document, 0, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 2.0);
	REQUIRE(Sample(document, 6, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 5.0);
	document.Tracks[0].End = "loop";
	REQUIRE(Sample(document, 6, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 3.0);
	document.Tracks[0].End = "ping";
	REQUIRE(Sample(document, 6, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 4.0);
	document.Tracks[0].End = "wrap";
	REQUIRE(Sample(document, 6, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(4.4));
	REQUIRE(Sample(document, 0, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(3.2));
}

TEST_CASE(
	"v4 source keys apply incoming cut before outgoing cut and authored Bezier handles", "[imagegraph]"
) {
	graph::Document document = NumberTrack(4);
	document.Keyframes = {SourceKey(2, 0.0), SourceKey(6, 8.0)};
	document.Tracks = {{"number", "value", "hold", -1}};
	graph::Diagnostic diagnostic;
	double value = 0;
	document.Keyframes[1].Ease->InType = "cut";
	document.Keyframes[0].Ease->OutType = "cut";
	REQUIRE(Sample(document, 4, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 0.0);
	document.Keyframes[1].Ease->InType = "bezier";
	REQUIRE(Sample(document, 4, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 8.0);
	document.Keyframes[0].Ease->OutType = "bezier";
	document.Keyframes[0].Ease->Out = {1.0 / 3, 0};
	document.Keyframes[1].Ease->In = {1.0 / 3, 0};
	REQUIRE(Sample(document, 4, value, diagnostic) == graph::Status::Ok);
	CHECK(value == Catch::Approx(1.0));
}

TEST_CASE("v4 wrap evaluates the closing segment at frame zero", "[imagegraph]") {
	graph::Document document = NumberTrack(4);
	document.Timeline = graph::TimelineSettings{6, 0, 5, "loop"};
	document.Keyframes = {SourceKey(0, 0.0), SourceKey(4, 4.0)};
	document.Keyframes[0].Ease->OutType = "cut";
	document.Tracks = {{"number", "value", "wrap", -1}};
	graph::Diagnostic diagnostic;
	double value = 0;
	REQUIRE(Sample(document, 0, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 0.0);
	REQUIRE(Sample(document, 1, value, diagnostic) == graph::Status::Ok);
	CHECK(value == 4.0);
}

TEST_CASE(
	"v4 timeline rejects missing side data, malformed ranges and unsupported interpolation", "[imagegraph]"
) {
	graph::Document document = NumberTrack(4);
	document.Keyframes = {SourceKey(0, 0.0), SourceKey(2, 2.0)};
	document.Tracks = {{"number", "value", "wrap", -1}};
	graph::Plan plan;
	graph::Diagnostic diagnostic;
	CHECK(graph::Compile(document, plan, diagnostic) == graph::Status::InvalidValue);
	document.Timeline = graph::TimelineSettings{3, 0, 2, "loop"};
	REQUIRE(graph::Compile(document, plan, diagnostic) == graph::Status::Ok);
	document.Keyframes[0].Ease.reset();
	CHECK(graph::Compile(document, plan, diagnostic) == graph::Status::InvalidValue);
	CHECK(diagnostic.NodeId == "number");
	document.Keyframes[0].Ease = graph::KeyframeEase{};
	document.Keyframes[0].Ease->OutType = "unknown";
	CHECK(graph::Compile(document, plan, diagnostic) == graph::Status::InvalidValue);
	document.Keyframes[0].Ease->OutType = "linear";
	document.Timeline->Frames = 0;
	CHECK(graph::Compile(document, plan, diagnostic) == graph::Status::InvalidValue);
	document.Timeline->Frames = 3;
	document.Tracks[0].End = "hold";
	document.Keyframes[1].Interpolation = "step";
	CHECK(graph::Compile(document, plan, diagnostic) == graph::Status::InvalidValue);
}
