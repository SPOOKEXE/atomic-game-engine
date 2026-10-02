#include <engine/imagegraph/AudioWindowPresentation.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.audio_window_presentation")
using namespace engine::imagegraph;
namespace {
	Document Graph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Timeline = TimelineSettings{8, 0, 7, "loop", 4};
		doc.Nodes = {
			{"file", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}, {"mono", false}}},
			{"window",
			 "pc.audio_window",
			 "",
			 {},
			 {{"width", int64_t{3}},
			  {"step", int64_t{1}},
			  {"cursor_location", EnumValue{0}},
			  {"location", 2.},
			  {"match_timeline", false}}}
		};
		doc.Links = {{"file", "data", "window", "audio_data"}};
		doc.Outputs = {{"samples", "window", "bit_array"}};
		return doc;
	}
	void Set(Document &doc, std::string_view port, Value data) {
		for (auto &input : doc.Nodes[1].Values)
			if (input.Port == port) {
				input.Data = std::move(data);
				return;
			}
		doc.Nodes[1].Values.push_back({std::string(port), std::move(data)});
	}
	const std::vector<AudioClipSource> CLIPS{
		{"clip",
		 {{},
		  8,
		  {{0, .125, .25, .375, .5, .625, .75, .875}, {0, -.125, -.25, -.375, -.5, -.625, -.75, -.875}}}}
	};
}
TEST_CASE(
	"Persisted Audio Window observation agrees with extraction in all cursor and unit modes",
	"[imagegraph][audio_window_presentation]"
) {
	EvaluationRequest request;
	request.AudioClips = CLIPS;
	for (int cursor = 0; cursor < 3; ++cursor)
		for (int unit = 0; unit < 3; ++unit) {
			auto doc = Graph();
			const double offset = cursor == 0 ? 2 : cursor == 1 ? 4 : 5;
			Set(doc, "cursor_location", EnumValue{cursor});
			Set(doc, "location_unit", EnumValue{unit});
			Set(doc, "location", unit == 0 ? offset : offset / 8);
			Document parsed;
			Diagnostic error;
			REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
			Plan plan;
			REQUIRE(Compile(parsed, plan, error) == Status::Ok);
			AudioWindowPresentation result;
			REQUIRE(
				ResolveAudioWindowPresentation(parsed, plan, "window", request, 100000, result, error) ==
				Status::Ok
			);
			CHECK(result.Start == .25);
			CHECK(result.End == .625);
			CHECK(result.Cursor == offset / 8);
			CHECK(result.Channels == 2);
			CHECK(result.Packets == 8);
			CHECK(result.SampleRate == 8);
			REQUIRE(result.Points.size() == 8);
			CHECK(result.Points.front() == Vector2{0, 0});
			CHECK(result.Points.back() == Vector2{.875, .875});
			EvaluatedValue value;
			REQUIRE(EvaluateValue(parsed, plan, "samples", request, value, error) == Status::Ok);
			const auto &rows = std::get<ArrayValue>(value.Data).Nested;
			REQUIRE(rows.size() == 2);
			CHECK(rows[0] == std::vector<ElementValue>{.25, .375, .5});
			CHECK(rows[1] == std::vector<ElementValue>{-.25, -.375, -.5});
		}
}
TEST_CASE(
	"Audio Window signed seek reuses geometry and failed refresh preserves last good artifact",
	"[imagegraph][audio_window_presentation]"
) {
	auto doc = Graph();
	Set(doc, "match_timeline", true);
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	auto clips = CLIPS;
	EvaluationRequest request;
	request.AudioClips = clips;
	REQUIRE(SetFrameTime(request, {1, 0, false}));
	AudioWindowPresentation result;
	REQUIRE(
		ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, result, error) == Status::Ok
	);
	const auto *points = result.Points.data();
	CHECK(result.Cursor == .25);
	REQUIRE(SetFrameTime(request, {1, .5, true}));
	REQUIRE(
		ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Points.data() == points);
	CHECK(result.Cursor == -.375);
	CHECK(result.Start == 0);
	CHECK(result.End == .375);
	const auto old = result;
	request.AudioClips = {};
	CHECK(
		ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, result, error) ==
		Status::InvalidValue
	);
	CHECK(result.Points.data() == points);
	CHECK(result.Cursor == old.Cursor);
	clips[0].Data.Channels[0].clear();
	clips[0].Data.Channels[1].clear();
	request.AudioClips = clips;
	REQUIRE(
		ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Points.empty());
	CHECK(result.Cursor == 0);
	CHECK(result.Start == 0);
	CHECK(result.End == 0);
}
TEST_CASE(
	"Audio Window replacement succeeds at its actual transaction cap and refuses one byte less",
	"[imagegraph][audio_window_presentation]"
) {
	auto doc = Graph();
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	auto clips = CLIPS;
	EvaluationRequest request;
	request.AudioClips = clips;
	AudioWindowPresentation old;
	REQUIRE(ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, old, error) == Status::Ok);
	clips[0].Data.Channels[0][0] = .5;
	uint64_t lower = 0, upper = 100000;
	while (lower + 1 < upper) {
		const auto cap = lower + (upper - lower) / 2;
		auto candidate = old;
		const auto code = ResolveAudioWindowPresentation(doc, plan, "window", request, cap, candidate, error);
		if (code == Status::Ok)
			upper = cap;
		else {
			REQUIRE(code == Status::LimitExceeded);
			lower = cap;
			CHECK(candidate.Points == old.Points);
		}
	}
	auto candidate = old;
	REQUIRE(
		ResolveAudioWindowPresentation(doc, plan, "window", request, upper, candidate, error) == Status::Ok
	);
	CHECK(candidate.Points.front().Y == .5);
	candidate = old;
	CHECK(
		ResolveAudioWindowPresentation(doc, plan, "window", request, upper - 1, candidate, error) ==
		Status::LimitExceeded
	);
	CHECK(candidate.Points == old.Points);
	CHECK(candidate.GeometryKey == old.GeometryKey);
}
TEST_CASE(
	"Recorded exact ticks and animated raw source controls drive the same observed bounds",
	"[imagegraph][audio_window_presentation]"
) {
	auto doc = Graph();
	doc.Nodes[0] = {"file", "image.audio_recording", "", {}, {{"source_id", std::string("music")}}};
	doc.Links[0].FromPort = "audio";
	Set(doc, "cursor_location", EnumValue{1});
	Set(doc, "location", 4.);
	Set(doc, "step", -.5);
	doc.Keyframes = {
		{"window", "width", 0, 2.5, "source", KeyframeEase{}},
		{"window", "width", 1, 3.5, "source", KeyframeEase{}}
	};
	doc.Tracks = {{"window", "width", "hold", -1}};
	std::vector<AudioCaptureFrame> frames{
		{"music", 0, {0, .125, .25, .375, .5, .625, .75, .875}, 8},
		{"music", 1, {0, .125, .25, .375, .5, .625, .75, .875}, 8}
	};
	Document parsed;
	Diagnostic error;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(parsed, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.AudioFrames = frames;
	AudioWindowPresentation result;
	REQUIRE(
		ResolveAudioWindowPresentation(parsed, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Start == .375);
	CHECK(result.End == .625);
	request.Tick = 1;
	REQUIRE(
		ResolveAudioWindowPresentation(parsed, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Start == .25);
	CHECK(result.End == .75);
	const auto old = result;
	request.Tick = 2;
	CHECK(
		ResolveAudioWindowPresentation(parsed, plan, "window", request, 100000, result, error) ==
		Status::InvalidValue
	);
	CHECK(result.Points == old.Points);
}

TEST_CASE(
	"Audio Window observation retains real source enum switches and step clamp",
	"[imagegraph][audio_window_presentation]"
) {
	auto doc = Graph();
	Set(doc, "step", int64_t{-4});
	Set(doc, "location", 4.);
	Set(doc, "cursor_location", .5);
	EvaluationRequest request;
	request.AudioClips = CLIPS;
	Diagnostic error;
	Plan plan;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	AudioWindowPresentation result;
	REQUIRE(
		ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Cursor == .5);
	CHECK(result.Start == 0);
	CHECK(result.End == .375);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(doc, plan, "samples", request, value, error) == Status::Ok);
	CHECK(std::get<ArrayValue>(value.Data).Nested[0] == std::vector<ElementValue>{0., .125, .25});
	Set(doc, "cursor_location", 3.);
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	REQUIRE(
		ResolveAudioWindowPresentation(doc, plan, "window", request, 100000, result, error) == Status::Ok
	);
	CHECK(result.Start == .125);
	CHECK(result.End == .5);
	REQUIRE(EvaluateValue(doc, plan, "samples", request, value, error) == Status::Ok);
	CHECK(std::get<ArrayValue>(value.Data).Nested[0] == std::vector<ElementValue>{.125, .25, .375});
}
