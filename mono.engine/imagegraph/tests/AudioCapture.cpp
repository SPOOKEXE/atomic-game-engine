#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.audio_capture")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;

	Document AudioVolumeDocument() {
		Document document;
		document.Nodes.push_back(
			{"capture",
			 "image.audio_recording",
			 "",
			 {},
			 {AuthoredValue{"source_id", std::string{"mono"}}},
			 {}}
		);
		document.Nodes.push_back({"volume", "image.audio_volume", "", {}, {}, {}});
		document.Links.push_back({"capture", "samples", "volume", "samples"});
		document.Outputs.push_back({"loudness", "volume", "loudness"});
		return document;
	}

	Status EvaluateLoudness(
		const Document &document,
		const Plan &plan,
		uint64_t tick,
		std::span<const AudioCaptureFrame> captures,
		EvaluatedValue &value,
		Diagnostic &diagnostic
	) {
		EvaluationRequest request;
		request.Tick = tick;
		request.AudioFrames = captures;
		return EvaluateValue(document, plan, "loudness", request, value, diagnostic);
	}

	Document AudioWindowDocument(int64_t cursor, int64_t width, double location, int64_t step) {
		Document document;
		document.FormatVersion = 6;
		document.Nodes = {
			{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}},
			{"window",
			 "image.audio_window",
			 "",
			 {},
			 {{"width", width},
			  {"location", location},
			  {"cursor_location", EnumValue{cursor}},
			  {"step", step},
			  {"match_timeline", false}},
			 {}},
		};
		document.Links = {{"capture", "samples", "window", "samples"}};
		document.Outputs = {{"window", "window", "samples"}};
		return document;
	}

	Status EvaluateWindow(
		const Document &document,
		const Plan &plan,
		std::span<const AudioCaptureFrame> captures,
		EvaluatedValue &value,
		Diagnostic &diagnostic,
		uint64_t tick = 0
	) {
		EvaluationRequest request;
		request.Tick = tick;
		request.AudioFrames = captures;
		return EvaluateValue(document, plan, "window", request, value, diagnostic);
	}
}

TEST_CASE("recorded mono audio text round trips exact bounded source frames", "[imagegraph][audio_capture]") {
	using namespace engine::imagegraph;
	const std::vector<AudioCaptureFrame> source{
		{"mono", 0, {1.0, -1.0}},
		{"mono", 1, {0.5, -0.5}},
		{"mono", 2, {0.25, -0.25}},
		{"mono", 3, {}},
	};
	std::string text;
	Diagnostic diagnostic;
	REQUIRE(WriteAudioCapture(source, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("audio-capture 1\n"));
	std::vector<AudioCaptureFrame> restored;
	REQUIRE(ReadAudioCapture(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == source);
	std::string secondText;
	REQUIRE(WriteAudioCapture(restored, secondText, diagnostic) == Status::Ok);
	CHECK(secondText == text);
}

TEST_CASE(
	"recorded mono audio v2 preserves the sample rate required by typed audio", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	const std::vector<AudioCaptureFrame> source{{"mono", 0, {1.0, -1.0}, 48'000.0}};
	std::string text;
	Diagnostic diagnostic;
	REQUIRE(WriteAudioCapture(source, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("audio-capture 2\n"));
	std::vector<AudioCaptureFrame> restored;
	REQUIRE(ReadAudioCapture(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == source);

	const std::vector<AudioCaptureFrame> incomplete{{"mono", 0, {1.0}, 48'000.0}, {"other", 0, {1.0}}};
	CHECK(WriteAudioCapture(incomplete, text, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "sample_rate");
}

TEST_CASE(
	"audio capture parsing leaves prior frames unchanged on malformed records", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	std::vector<AudioCaptureFrame> frames{{"keep", 9, {0.75}}};
	const auto unchanged = frames;
	Diagnostic diagnostic;
	CHECK(
		ReadAudioCapture("audio-capture 1\nframe \"mono\" 0 1 nan\n", frames, diagnostic) == Status::Malformed
	);
	CHECK(frames == unchanged);
	CHECK(
		ReadAudioCapture("audio-capture 1\nframe \"bad/id\" 0 0\n", frames, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(frames == unchanged);
}

TEST_CASE(
	"audio capture limits bound source frames and decoded sample storage", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	Diagnostic diagnostic;
	std::vector<AudioCaptureFrame> tooManyFrames(Limits::MaximumAudioCaptureFrames + 1);
	CHECK(ValidateAudioCaptureFrames(tooManyFrames, diagnostic) == Status::LimitExceeded);

	const std::vector<AudioCaptureFrame> tooManySamples{
		{"mono", 0, std::vector<double>(Limits::MaximumAudioSamplesPerFrame + 1)}
	};
	CHECK(ValidateAudioCaptureFrames(tooManySamples, diagnostic) == Status::LimitExceeded);
	std::string previousText = "preserve";
	CHECK(WriteAudioCapture(tooManySamples, previousText, diagnostic) == Status::LimitExceeded);
	CHECK(previousText == "preserve");

	std::vector<AudioCaptureFrame> tooManyTotalSamples;
	const size_t aggregateLimitFrameCount =
		Limits::MaximumAudioCaptureSamples / Limits::MaximumAudioSamplesPerFrame + 1;
	tooManyTotalSamples.reserve(aggregateLimitFrameCount);
	for (size_t index = 0; index < aggregateLimitFrameCount; index++)
		tooManyTotalSamples.push_back(
			{"mono", static_cast<uint64_t>(index), std::vector<double>(Limits::MaximumAudioSamplesPerFrame)}
		);
	CHECK(ValidateAudioCaptureFrames(tooManyTotalSamples, diagnostic) == Status::LimitExceeded);

	const std::vector<AudioCaptureFrame> pastMaximumTick{{"mono", Limits::MaximumTick + 1, {}}};
	CHECK(ValidateAudioCaptureFrames(pastMaximumTick, diagnostic) == Status::LimitExceeded);

	std::vector<AudioCaptureFrame> existing{{"keep", 7, {0.5}}};
	const auto unchanged = existing;
	const std::string oversizedText(Limits::MaximumAudioCaptureDocumentBytes + 1, 'x');
	CHECK(ReadAudioCapture(oversizedText, existing, diagnostic) == Status::LimitExceeded);
	CHECK(existing == unchanged);

	std::string aggregateOverflow = "audio-capture 1\n";
	for (size_t tick = 0; tick < aggregateLimitFrameCount; tick++) {
		aggregateOverflow += "frame \"mono\" " + std::to_string(tick) + " " +
							 std::to_string(Limits::MaximumAudioSamplesPerFrame);
		for (size_t sample = 0; sample < Limits::MaximumAudioSamplesPerFrame; sample++)
			aggregateOverflow += " 0";
		aggregateOverflow += '\n';
	}
	REQUIRE(aggregateOverflow.size() < Limits::MaximumAudioCaptureDocumentBytes);
	CHECK(ReadAudioCapture(aggregateOverflow, existing, diagnostic) == Status::LimitExceeded);
	CHECK(existing == unchanged);
}

TEST_CASE(
	"Audio Volume evaluates exact capture ticks and replays the same scalar", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	const Document document = AudioVolumeDocument();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> captures{
		{"mono", 0, {1.0, -1.0}},
		{"mono", 1, {0.5, -0.5}},
		{"mono", 2, {0.25, -0.25}},
		{"mono", 3, {}},
	};
	const std::vector<double> expected{
		0.0,
		10.0 * std::log10(0.5),
		10.0 * std::log10(0.25),
		0.0,
	};
	for (uint64_t tick = 0; tick < captures.size(); tick++) {
		EvaluatedValue value;
		REQUIRE(EvaluateLoudness(document, plan, tick, captures, value, diagnostic) == Status::Ok);
		REQUIRE(value.Port == "loudness");
		const auto *loudness = std::get_if<double>(&value.Data);
		REQUIRE(loudness != nullptr);
		CHECK(*loudness == Catch::Approx(expected[static_cast<size_t>(tick)]).epsilon(1e-14));
		EvaluatedValue replay;
		REQUIRE(EvaluateLoudness(document, plan, tick, captures, replay, diagnostic) == Status::Ok);
		CHECK(replay == value);
	}
}

TEST_CASE(
	"recorded audio requires an exact source and tick and rejects duplicate keys",
	"[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	const Document document = AudioVolumeDocument();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> captures{{"mono", 0, {1.0}}};
	EvaluatedValue value;
	CHECK(EvaluateLoudness(document, plan, 1, captures, value, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "capture");
	CHECK(diagnostic.Port == "source_id");

	const std::vector<AudioCaptureFrame> duplicate{{"mono", 0, {1.0}}, {"mono", 0, {0.5}}};
	CHECK(ValidateAudioCaptureFrames(duplicate, diagnostic) == Status::DuplicateId);
}

TEST_CASE(
	"Audio Volume maps nonempty silence to a finite-contract diagnostic", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	const Document document = AudioVolumeDocument();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> captures{{"mono", 0, {0.0, 0.0}}};
	EvaluatedValue value;
	CHECK(EvaluateLoudness(document, plan, 0, captures, value, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Message.find("nonempty silent audio") != std::string::npos);
	CHECK(diagnostic.Message.find("10*log10(RMS)") != std::string::npos);

	const std::vector<AudioCaptureFrame> nonFinite{{"mono", 0, {std::numeric_limits<double>::infinity()}}};
	CHECK(ValidateAudioCaptureFrames(nonFinite, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Audio Window takes a bounded static scalar slice with cursor and step", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	std::vector<double> source;
	for (double sample = 0; sample < 16; ++sample)
		source.push_back(sample);
	const std::vector<AudioCaptureFrame> captures{{"mono", 0, std::move(source)}};
	for (const auto &[cursor, location, expected] : std::array{
			 std::tuple{int64_t{0}, 9.0, std::array<int64_t, 4>{8, 10, 12, 14}},
			 std::tuple{int64_t{1}, 2.0, std::array<int64_t, 4>{0, 2, 4, 6}},
			 std::tuple{int64_t{2}, 9.0, std::array<int64_t, 4>{1, 3, 5, 7}},
		 }) {
		const Document document = AudioWindowDocument(cursor, 8, location, 2);
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue value;
		REQUIRE(EvaluateWindow(document, plan, captures, value, diagnostic) == Status::Ok);
		const auto *window = std::get_if<ArrayValue>(&value.Data);
		REQUIRE(window != nullptr);
		REQUIRE(window->ElementType == ValueType::Scalar);
		REQUIRE(window->Elements.size() == expected.size());
		for (size_t index = 0; index < expected.size(); ++index)
			CHECK(std::get<double>(window->Elements[index]) == static_cast<double>(expected[index]));
	}
	const Document uneven = AudioWindowDocument(0, 7, 1.0, 2);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(uneven, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateWindow(uneven, plan, captures, value, diagnostic) == Status::Ok);
	const auto *window = std::get_if<ArrayValue>(&value.Data);
	REQUIRE(window != nullptr);
	REQUIRE(window->Elements.size() == 4);
	CHECK(std::get<double>(window->Elements[0]) == 1.0);
	CHECK(std::get<double>(window->Elements[3]) == 7.0);
}

TEST_CASE(
	"Audio Window rejects timeline mode and invalid extraction controls", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	Document document = AudioWindowDocument(1, 8, 4.0, 2);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	const std::vector<AudioCaptureFrame> captures{{"mono", 0, {0.0, 1.0, 2.0, 3.0}}};
	document.Nodes[1].Values.back().Data = true;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateWindow(document, plan, captures, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "window");
	CHECK(diagnostic.Port == "audio");

	document.Nodes[1].Values.back().Data = false;
	document.Nodes[1].Values[3].Data = int64_t{0};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateWindow(document, plan, captures, value, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "step");
}

TEST_CASE(
	"Audio Window Match Timeline uses typed audio sample rate and declared FPS", "[imagegraph][audio_capture]"
) {
	using namespace engine::imagegraph;
	Document document = AudioWindowDocument(0, 4, 0.0, 2);
	document.Nodes[1].Values.back().Data = true;
	document.Links = {{"capture", "audio", "window", "audio"}};
	document.Timeline = TimelineSettings{8, 0, 7, "loop", 2.0};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::vector<double> source;
	for (double sample = 0; sample < 16; ++sample)
		source.push_back(sample);
	const std::vector<AudioCaptureFrame> captures{{"mono", 4, std::move(source), 4.0}};
	EvaluatedValue value;
	REQUIRE(EvaluateWindow(document, plan, captures, value, diagnostic, 4) == Status::Ok);
	const auto *window = std::get_if<ArrayValue>(&value.Data);
	REQUIRE(window != nullptr);
	REQUIRE(window->Elements.size() == 2);
	CHECK(std::get<double>(window->Elements[0]) == 8.0);
	CHECK(std::get<double>(window->Elements[1]) == 10.0);

	document.Timeline.reset();
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateWindow(document, plan, captures, value, diagnostic, 4) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "match_timeline");
}
