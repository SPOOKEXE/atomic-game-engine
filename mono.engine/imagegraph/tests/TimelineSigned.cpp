#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.timeline.signed")
using namespace engine::imagegraph;
namespace {
	Document ClockGraph(double first, double last, const std::string &interpolation = "linear") {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {{"n", "value.number", "", {}, {{"value", 0.}}}};
		d.Outputs = {{"out", "n", "number"}};
		d.Keyframes = {{"n", "value", 0, 0., interpolation}, {"n", "value", 0, 10., interpolation}};
		FrameTime a, b;
		REQUIRE(SplitFrameTime(first, a));
		REQUIRE(SplitFrameTime(last, b));
		REQUIRE(SetFrameTime(d.Keyframes[0], a));
		REQUIRE(SetFrameTime(d.Keyframes[1], b));
		if (interpolation == "source") d.Tracks = {{"n", "value", "hold", -1}};
		if (interpolation == "source")
			for (auto &k : d.Keyframes)
				k.Ease = KeyframeEase{};
		return d;
	}
	double Sample(Document d, double frame) {
		Diagnostic diagnostic;
		Document parsed;
		REQUIRE(Read(Write(d), parsed, diagnostic) == Status::Ok);
		Plan plan;
		INFO(diagnostic.Message);
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		EvaluationRequest r;
		FrameTime t;
		REQUIRE(SplitFrameTime(frame, t));
		REQUIRE(SetFrameTime(r, t));
		EvaluatedValue result;
		auto status = EvaluateValue(parsed, plan, "out", r, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return std::get<double>(result.Data);
	}
}
TEST_CASE(
	"Signed native keys persist exact fractions and interpolate through zero", "[imagegraph][timeline_signed]"
) {
	auto d = ClockGraph(.25, .75);
	CHECK(Sample(d, 0) == 0);
	CHECK(Sample(d, .25) == 0);
	CHECK(Sample(d, .5) == 5);
	CHECK(Sample(d, .75) == 10);
	CHECK(Sample(d, 1) == 10);
	d = ClockGraph(-2, -.5);
	CHECK(Sample(d, -3) == 0);
	CHECK(Sample(d, -1.25) == 5);
	CHECK(Sample(d, 0) == 10);
	d = ClockGraph(-.5, .5);
	CHECK(Sample(d, 0) == 5);
	d.FormatVersion = 8;
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(d, plan, diagnostic) == Status::UnsupportedVersion);
	d.FormatVersion = 9;
	d.Keyframes[1] = d.Keyframes[0];
	CHECK(Compile(d, plan, diagnostic) == Status::DuplicateId);
}
TEST_CASE(
	"Source signed sampling keeps its verified nonpositive first-key hold", "[imagegraph][timeline_signed]"
) {
	auto d = ClockGraph(-2, .5, "source");
	CHECK(Sample(d, -1.5) == 0);
	CHECK(Sample(d, -.5) == 0);
	CHECK(Sample(d, 0) == 0);
	CHECK(Sample(d, .5) == 10);
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluationRequest r;
	r.Subframe = .25;
	EvaluatedValue result;
	result.Data = 99.;
	CHECK(EvaluateValue(d, plan, "out", r, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Message == "source fractional key-map index coercion is unverified");
	CHECK(std::get<double>(result.Data) == 99.);
}
TEST_CASE(
	"Signed key metadata parsing is atomic and default records stay optional", "[imagegraph][timeline_signed]"
) {
	auto d = ClockGraph(-2.5, .75);
	Diagnostic diagnostic;
	Document parsed;
	const auto text = Write(d);
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(GetFrameTime(parsed.Keyframes[0]) == GetFrameTime(d.Keyframes[0]));
	CHECK(GetFrameTime(parsed.Keyframes[1]) == GetFrameTime(d.Keyframes[1]));
	const Document sentinel = parsed;
	auto invalid = text;
	const auto position = invalid.find("negative");
	REQUIRE(position != std::string::npos);
	invalid.replace(position, 8, "unknown");
	CHECK(Read(invalid, parsed, diagnostic) == Status::Malformed);
	CHECK(parsed == sentinel);
	auto legacy = ClockGraph(0, 2);
	CHECK(Write(legacy).find("key_time") == std::string::npos);
	for (uint32_t version = 1; version <= 8; ++version) {
		legacy.FormatVersion = version;
		const auto old = Write(legacy);
		REQUIRE(Read(old, parsed, diagnostic) == Status::Ok);
		CHECK(Write(parsed) == old);
	}
}

TEST_CASE("Signed native tracks select loop ping and wrap intervals", "[imagegraph][timeline_signed]") {
	auto d = ClockGraph(-2, .5);
	d.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	d.Tracks = {{"n", "value", "loop", -1}};
	CHECK(Sample(d, 1) == 2);
	d.Tracks.front().End = "ping";
	CHECK(Sample(d, 1) == 8);
	d = ClockGraph(-.5, .5);
	d.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	d.Tracks = {{"n", "value", "wrap", -1}};
	CHECK(std::abs(Sample(d, 1) - 10. * 5. / 6.) < 1e-12);
}
TEST_CASE(
	"Single source driver consumes the signed global frame after its key", "[imagegraph][timeline_signed]"
) {
	auto d = ClockGraph(-2, 2, "source");
	d.Keyframes.resize(1);
	d.Keyframes.front().SourceDriver = KeyframeLinearDriver{2};
	CHECK(Sample(d, -3) == 0);
	CHECK(Sample(d, -1) == -2);
	CHECK(Sample(d, 1) == 2);
}
TEST_CASE(
	"Signed Audio Window author clocks keep unsigned capture lookup",
	"[imagegraph][timeline_signed][node_audio]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Timeline = TimelineSettings{8, 0, 7, "loop", 10};
	d.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"clip"}}}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"step", int64_t{1}}, {"cursor_location", EnumValue{0}}}}
	};
	d.Links = {{"capture", "audio", "window", "audio_data"}};
	d.Outputs = {{"out", "window", "bit_array"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{{"clip", 1, {1, 2, 3, 4, 5}, 10}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	REQUIRE(SetFrameTime(request, {1, .5, true}));
	EvaluatedValue value, replay;
	auto status = EvaluateValue(d, plan, "out", request, value, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &a = std::get<ArrayValue>(value.Data);
	REQUIRE(a.Nested.size() == 1);
	REQUIRE(a.Nested.front().size() == 2);
	CHECK(std::get<double>(a.Nested[0][0]) == 1);
	CHECK(std::get<double>(a.Nested[0][1]) == 2);
	REQUIRE(EvaluateValue(d, plan, "out", request, replay, diagnostic) == Status::Ok);
	CHECK(value.Data == replay.Data);
	request.Tick = 2;
	CHECK(EvaluateValue(d, plan, "out", request, replay, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Native signed wrap intervals and source nonpositive extrapolation remain distinct",
	"[imagegraph][timeline_signed]"
) {
	auto d = ClockGraph(-2, .5);
	d.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	d.Tracks = {{"n", "value", "wrap", -1}};
	CHECK(Sample(d, -1.5) == 2);
	CHECK(Sample(d, 0) == 8);
	d = ClockGraph(-2, 1, "source");
	d.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	d.Tracks = {{"n", "value", "wrap", -1}};
	CHECK(Sample(d, -1.5) == -5);
	CHECK(Sample(d, 0) == -20);
	d.Keyframes.back().Ease->OutType = "bezier";
	CHECK(Sample(d, -1.5) == 0);
	d.Keyframes.front().Ease->InType = "cut";
	CHECK(Sample(d, -1.5) == 10);
}
TEST_CASE(
	"Single native wrap keeps its frame boundary and legacy fractional hold", "[imagegraph][timeline_signed]"
) {
	auto d = ClockGraph(.25, 1);
	d.Keyframes.resize(1);
	d.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	d.Tracks = {{"n", "value", "wrap", -1}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 4;
	EvaluatedValue value;
	value.Data = 99.;
	CHECK(EvaluateValue(d, plan, "out", request, value, diagnostic) == Status::InvalidValue);
	CHECK(std::get<double>(value.Data) == 99.);
	CHECK(Sample(d, 3) == 0);
	d = ClockGraph(0, 2);
	d.FormatVersion = 8;
	CHECK(Sample(d, .5) == 0);
	d.FormatVersion = 9;
	CHECK(Sample(d, .5) == 2.5);
	// The source's single-key getter bypasses wrap range selection entirely.
	d = ClockGraph(-2, 1, "source");
	d.Keyframes.resize(1);
	d.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	d.Tracks = {{"n", "value", "wrap", -1}};
	d.Keyframes.front().SourceDriver = KeyframeLinearDriver{2};
	CHECK(Sample(d, 4) == 8);
}
