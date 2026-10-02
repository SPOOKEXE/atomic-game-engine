#include "AudioKeyDriver.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.timeline.audio")
using namespace engine::imagegraph;

TEST_CASE(
	"Captured audio drivers persist and replay fractional seeks by exact capture tick",
	"[imagegraph][timeline_audio]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {
		{"number", "value", 0, 1.0, "source", KeyframeEase{}},
		{"number", "value", 4, 5.0, "source", KeyframeEase{}}
	};
	for (auto &key : document.Keyframes)
		key.SourceDriver = KeyframeAudioDriver{"clip", "rms", 0, 2, -1};
	document.Tracks = {{"number", "value", "hold", -1}};
	Diagnostic diagnostic;
	Document parsed;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	Plan plan;
	const Status compiled = Compile(parsed, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	std::vector<AudioCaptureFrame> frames{{"clip", 0, {1, -1}}, {"clip", 1, {3, -3}}, {"clip", 3, {1, -1}}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	EvaluatedValue result;
	for (const uint64_t tick : {1, 3, 1}) {
		request.Tick = tick;
		request.Subframe = .5;
		REQUIRE(EvaluateValue(parsed, plan, "out", request, result, diagnostic) == Status::Ok);
		CHECK(std::get<double>(result.Data) == (tick == 1 ? 7.5 : 5.5));
	}
	request.Tick = 0;
	request.Subframe = 0;
	REQUIRE(EvaluateValue(parsed, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 2);
	const auto prior = result;
	request.Tick = 2;
	CHECK(EvaluateValue(parsed, plan, "out", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result == prior);
}

TEST_CASE(
	"Audio driver metrics admit silence channel selection and finite extreme samples",
	"[imagegraph][timeline_audio]"
) {
	using namespace engine::imagegraph::detail;
	KeyframeAudioDriver driver{"clip", "rms", 0, 1, 0};
	std::vector<AudioCaptureFrame> frames{{"clip", 0, {0, 0}}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	double value = 7;
	REQUIRE(ResolveAudioKeyOffset(driver, request, value) == Status::Ok);
	CHECK(value == 0);
	frames[0].Samples = {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
	REQUIRE(ResolveAudioKeyOffset(driver, request, value) == Status::Ok);
	CHECK(value == std::numeric_limits<double>::max());
	driver.Metric = "mean";
	REQUIRE(ResolveAudioKeyOffset(driver, request, value) == Status::Ok);
	CHECK(value == 0);
	frames[0].Samples.clear();
	frames[0].Channels = {{1, -1}, {3, -4}};
	frames[0].SampleRate = 48000;
	driver.Channel = 1;
	driver.Metric = "peak";
	REQUIRE(ResolveAudioKeyOffset(driver, request, value) == Status::Ok);
	CHECK(value == 4);
	driver.Channel = 2;
	CHECK(ResolveAudioKeyOffset(driver, request, value) == Status::InvalidValue);
	driver.Channel = 1;
	frames.push_back(frames.front());
	request.AudioFrames = frames;
	CHECK(ResolveAudioKeyOffset(driver, request, value) == Status::DuplicateId);
	frames.pop_back();
	request.AudioFrames = frames;
	driver.Metric = "guessed";
	CHECK_FALSE(ValidAudioKeyDriver(driver));
	driver.Metric = "peak";
	driver.Gain = std::numeric_limits<double>::infinity();
	CHECK_FALSE(ValidAudioKeyDriver(driver));
}
