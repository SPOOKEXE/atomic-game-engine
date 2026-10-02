// The FFT fixtures distinguish the default extension from the GML fallback, then replay captured audio.

#include "NodeHarness.hpp"

#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

TEST_SUITE_ID("engine.imagegraph.node_audio")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	ArrayValue Samples(std::initializer_list<double> samples) {
		ArrayValue data{ValueType::Scalar, {}};
		for (double sample : samples)
			data.Elements.emplace_back(sample);
		return data;
	}

	ArrayValue DirectSpectrum(const ArrayValue &data, int window) {
		const size_t count = data.Elements.size();
		size_t padded = 1;
		while (padded < count)
			padded *= 2;
		ArrayValue result{ValueType::Scalar, {}};
		for (size_t bin = padded / 2 + 1; bin > 0; bin--) {
			std::complex<double> sum{};
			for (size_t index = 0; index < count; index++) {
				const double phase = 2 * std::numbers::pi * index / count;
				const double gain = window == 0	  ? 1
									: window == 1 ? .5 * (1 - std::cos(phase))
												  : .42 - .5 * std::cos(phase) + .08 * std::cos(2 * phase);
				const double angle = -2 * std::numbers::pi * (bin - 1) * index / padded;
				sum += std::get<double>(data.Elements[index]) * gain *
					   std::complex<double>{std::cos(angle), std::sin(angle)};
			}
			result.Elements.emplace_back(static_cast<double>(static_cast<float>(std::abs(sum))));
		}
		return result;
	}
}

TEST_CASE("FFT uses periodic windows and reverses the padded half spectrum", "[imagegraph][node_audio]") {
	const ArrayValue data = Samples({.2, -.7, .9, .3, -.1});
	for (int window = 0; window < 3; window++) {
		const auto run = RunNode("pc.fft", {}, {{"data", data}, {"preprocess_function", EnumValue{window}}});
		REQUIRE(run.Ok);
		const auto *output = run.OutputValue("array");
		REQUIRE(output);
		const auto &actual = std::get<ArrayValue>(*output);
		const ArrayValue expected = DirectSpectrum(data, window);
		REQUIRE(actual.Elements.size() == 5);
		for (size_t index = 0; index < actual.Elements.size(); index++)
			CHECK(
				std::abs(
					std::get<double>(actual.Elements[index]) - std::get<double>(expected.Elements[index])
				) < 1e-6
			);
	}
	const auto dc = RunNode("pc.fft", {}, {{"data", Samples({1, 1, 1, 1})}});
	REQUIRE(dc.Ok);
	CHECK(std::get<ArrayValue>(*dc.OutputValue("array")) == Samples({0, 0, 4}));
}

TEST_CASE("FFT handles trivial samples and rejects invalid or unbounded work", "[imagegraph][node_audio]") {
	for (const auto &data : {Samples({}), Samples({1})}) {
		const auto run = RunNode("pc.fft", {}, {{"data", data}});
		REQUIRE(run.Ok);
		CHECK(std::get<ArrayValue>(*run.OutputValue("array")).Elements.empty());
	}
	const auto clampedWindow =
		RunNode("pc.fft", {}, {{"data", Samples({1, 0, -1, 0})}, {"preprocess_function", EnumValue{3}}});
	REQUIRE(clampedWindow.Ok);
	CHECK(
		std::get<ArrayValue>(*clampedWindow.OutputValue("array")) == DirectSpectrum(Samples({1, 0, -1, 0}), 2)
	);
	for (const auto &data :
		 {ArrayValue{ValueType::Integer, {int64_t{1}}},
		  Samples({std::numeric_limits<double>::quiet_NaN()}),
		  Samples({1e308, 1e308})}) {
		const auto run = RunNode("pc.fft", {}, {{"data", data}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == "data");
	}
	ArrayValue large{ValueType::Scalar, std::vector<ElementValue>(Limits::MaximumArrayElements + 1, 0.0)};
	CHECK(RunNode("pc.fft", {}, {{"data", large}}).Code == Status::LimitExceeded);
	const auto *entry = FindCatalogueEntry("pc.fft");
	REQUIRE(entry);
	Node node{"fft", "pc.fft", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.Values.emplace_back("data", Samples({1, 2, 3}));
	context.ByteBudget = 1;
	CHECK_FALSE(detail::FindExecutor("pc.fft")(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
	large.Elements.resize(Limits::MaximumArrayElements);
	const auto maximum = RunNode("pc.fft", {}, {{"data", large}});
	REQUIRE(maximum.Ok);
	CHECK(std::get<ArrayValue>(*maximum.OutputValue("array")).Elements.size() == 2049);
}

TEST_CASE(
	"captured audio windows feed FFT with deterministic replay and missing-tick diagnostics",
	"[imagegraph][node_audio]"
) {
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}},
		{"window",
		 "image.audio_window",
		 "",
		 {},
		 {{"width", int64_t{4}},
		  {"step", int64_t{1}},
		  {"cursor_location", EnumValue{0}},
		  {"match_timeline", false}},
		 {}},
		{"fft", "pc.fft", "", {}, {}, {}},
	};
	document.Links = {{"capture", "samples", "window", "samples"}, {"window", "samples", "fft", "data"}};
	document.Outputs = {{"spectrum", "fft", "array"}};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{{"mono", 0, {1, 1, 1, 1}}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	EvaluatedValue first, replay;
	REQUIRE(EvaluateValue(document, plan, "spectrum", request, first, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "spectrum", request, replay, diagnostic) == Status::Ok);
	CHECK(first.Data == replay.Data);
	CHECK(std::get<ArrayValue>(first.Data) == Samples({0, 0, 4}));
	request.Tick = 1;
	CHECK(EvaluateValue(document, plan, "spectrum", request, replay, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "capture");
}

TEST_CASE(
	"Audio Volume follows pinned RMS before log and handles empty mono samples", "[imagegraph][node_audio]"
) {
	const auto empty = RunNode("pc.audio_loudness", {}, {{"audio_data", Samples({})}});
	REQUIRE(empty.Ok);
	CHECK(std::get<double>(*empty.OutputValue("loudness")) == 0);
	const auto unit = RunNode("pc.audio_loudness", {}, {{"audio_data", Samples({1, -1})}});
	REQUIRE(unit.Ok);
	CHECK(std::get<double>(*unit.OutputValue("loudness")) == 0);
	const auto quarter = RunNode("pc.audio_loudness", {}, {{"audio_data", Samples({.25, -.25})}});
	REQUIRE(quarter.Ok);
	CHECK(std::abs(std::get<double>(*quarter.OutputValue("loudness")) - 10 * std::log10(.25)) < 1e-12);
	const auto asymmetric = RunNode("pc.audio_loudness", {}, {{"audio_data", Samples({0, 2, 0, -2})}});
	REQUIRE(asymmetric.Ok);
	CHECK(
		std::abs(std::get<double>(*asymmetric.OutputValue("loudness")) - 10 * std::log10(std::sqrt(2))) <
		1e-12
	);
	const auto defaultValue = RunNode("pc.audio_loudness", {});
	REQUIRE(defaultValue.Ok);
	CHECK(std::get<double>(*defaultValue.OutputValue("loudness")) == 0);
}

TEST_CASE(
	"Audio Volume reports non-finite and unbounded inputs on its source port", "[imagegraph][node_audio]"
) {
	for (const auto &data :
		 {Samples({0, 0}),
		  Samples({1e308}),
		  Samples({std::numeric_limits<double>::quiet_NaN()}),
		  ArrayValue{ValueType::Scalar, {std::string{"bad"}}},
		  ArrayValue{ValueType::Integer, {int64_t{1}}}}) {
		const auto run = RunNode("pc.audio_loudness", {}, {{"audio_data", data}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == "audio_data");
		CHECK(run.Values.empty());
	}
	ArrayValue large{ValueType::Scalar, std::vector<ElementValue>(Limits::MaximumArrayElements + 1, 1.0)};
	CHECK(RunNode("pc.audio_loudness", {}, {{"audio_data", large}}).Code == Status::LimitExceeded);
	large.Elements.resize(Limits::MaximumArrayElements);
	const auto maximum = RunNode("pc.audio_loudness", {}, {{"audio_data", large}});
	REQUIRE(maximum.Ok);
	CHECK(std::get<double>(*maximum.OutputValue("loudness")) == 0);
}

TEST_CASE(
	"captured audio drives catalogue loudness and replays at exact source ticks", "[imagegraph][node_audio]"
) {
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}},
		{"volume", "pc.audio_loudness", "", {}, {}, {}},
	};
	document.Links = {{"capture", "samples", "volume", "audio_data"}};
	document.Outputs = {{"loudness", "volume", "loudness"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{{"mono", 7, {.25, -.25}}, {"mono", 8, {0, 0}}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	request.Tick = 7;
	EvaluatedValue first, replay;
	REQUIRE(EvaluateValue(document, plan, "loudness", request, first, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "loudness", request, replay, diagnostic) == Status::Ok);
	CHECK(first.Data == replay.Data);
	CHECK(std::abs(std::get<double>(first.Data) - 10 * std::log10(.25)) < 1e-12);
	request.Tick = 8;
	CHECK(EvaluateValue(document, plan, "loudness", request, replay, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "volume");
	CHECK(diagnostic.Port == "audio_data");
	request.Tick = 9;
	CHECK(EvaluateValue(document, plan, "loudness", request, replay, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "capture");
}

TEST_CASE(
	"catalogue Audio Window preserves channels and follows source cursor bounds and step clamp",
	"[imagegraph][node_audio]"
) {
	const AudioBit audio{{}, 10, {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {10, 11, 12, 13, 14, 15, 16, 17, 18, 19}}};
	for (const auto [cursor, location, width, step, first, last] :
		 {std::array<int64_t, 6>{0, 0, 4, 1, 0, 3},
		  {0, 9, 4, 1, 5, 8},
		  {1, 4, 3, 1, 2, 4},
		  {2, 4, 3, 1, 1, 3},
		  {0, -100, 4, 0, 0, 3},
		  {0, 0, 4, -5, 0, 3},
		  {0, 0, 4, 3, 0, 3},
		  {0, 100, 4096, 1, 0, 8}}) {
		const auto run = RunNode(
			"pc.audio_window",
			{},
			{{"audio_data", audio},
			 {"width", width},
			 {"location", static_cast<double>(location)},
			 {"cursor_location", EnumValue{cursor}},
			 {"step", step},
			 {"match_timeline", false}}
		);
		REQUIRE(run.Ok);
		const auto &value = std::get<ArrayValue>(*run.OutputValue("bit_array"));
		CHECK(value.Elements.empty());
		REQUIRE(value.Nested.size() == 2);
		for (size_t channel = 0; channel < 2; channel++) {
			CHECK(std::get<double>(value.Nested[channel].front()) == first + channel * 10);
			CHECK(std::get<double>(value.Nested[channel].back()) == last + channel * 10);
		}
	}
	for (const AudioBit &source : {AudioBit{{}, 10}, AudioBit{{1}, 10}}) {
		const auto run = RunNode("pc.audio_window", {}, {{"audio_data", source}, {"match_timeline", false}});
		REQUIRE(run.Ok);
		const auto &value = std::get<ArrayValue>(*run.OutputValue("bit_array"));
		REQUIRE(value.Nested.size() == 1);
		CHECK(value.Nested.front().empty());
	}
	const auto zeroWidth = RunNode(
		"pc.audio_window", {}, {{"audio_data", audio}, {"width", int64_t{0}}, {"match_timeline", false}}
	);
	REQUIRE(zeroWidth.Ok);
	CHECK(
		std::get<ArrayValue>(*zeroWidth.OutputValue("bit_array")).Nested ==
		std::vector<std::vector<ElementValue>>(2)
	);
}

TEST_CASE(
	"Audio Window location units and timeline use declared source rate and FPS", "[imagegraph][node_audio]"
) {
	const AudioBit audio{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 10};
	for (int64_t unit = 0; unit < 3; unit++) {
		const auto run = RunNode(
			"pc.audio_window",
			{},
			{{"audio_data", audio},
			 {"width", int64_t{4}},
			 {"location", unit == 0 ? 4.0 : .4},
			 {"location_unit", EnumValue{unit}},
			 {"cursor_location", EnumValue{0}},
			 {"step", int64_t{1}},
			 {"match_timeline", false}}
		);
		REQUIRE(run.Ok);
		CHECK(
			std::get<ArrayValue>(*run.OutputValue("bit_array")).Nested.front() ==
			Samples({4, 5, 6, 7}).Elements
		);
	}
	TimelineSettings timeline{10, 0, 9, "loop", 10};
	const auto timed = RunNode(
		"pc.audio_window",
		{},
		{{"audio_data", audio},
		 {"width", int64_t{4}},
		 {"location", 100.0},
		 {"cursor_location", EnumValue{0}},
		 {"step", int64_t{1}}},
		2,
		0,
		&timeline
	);
	REQUIRE(timed.Ok);
	CHECK(
		std::get<ArrayValue>(*timed.OutputValue("bit_array")).Nested.front() == Samples({2, 3, 4, 5}).Elements
	);
	timeline.FramesPerSecond = 20;
	const auto changedFps = RunNode(
		"pc.audio_window",
		{},
		{{"audio_data", audio},
		 {"width", int64_t{4}},
		 {"cursor_location", EnumValue{0}},
		 {"step", int64_t{1}}},
		2,
		0,
		&timeline
	);
	REQUIRE(changedFps.Ok);
	CHECK(
		std::get<ArrayValue>(*changedFps.OutputValue("bit_array")).Nested.front() ==
		Samples({1, 2, 3, 4}).Elements
	);
	CHECK(RunNode("pc.audio_window", {}, {{"audio_data", audio}}).Code == Status::UnsupportedExecution);
	for (const AudioBit &bad :
		 {AudioBit{{1}, 0},
		  AudioBit{{1}, 10, {{1}}},
		  AudioBit{{}, 10, {{1}, {1, 2}}},
		  AudioBit{{std::numeric_limits<double>::infinity()}, 10}}) {
		const auto run = RunNode("pc.audio_window", {}, {{"audio_data", bad}, {"match_timeline", false}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Port == "audio_data");
	}
	const auto *entry = FindCatalogueEntry("pc.audio_window");
	REQUIRE(entry);
	Node node{"window", "pc.audio_window", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.Values = {{"audio_data", audio}, {"match_timeline", false}};
	CHECK_FALSE(detail::FindExecutor("pc.audio_window")(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
}

TEST_CASE(
	"planar capture drives source Audio Window and per-channel audio processors", "[imagegraph][node_audio]"
) {
	Document document;
	document.FormatVersion = 6;
	document.Timeline = TimelineSettings{10, 0, 9, "loop", 10};
	document.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"stereo"}}}, {}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t{4}}, {"cursor_location", EnumValue{0}}, {"step", int64_t{1}}},
		 {}},
		{"fft", "pc.fft", "", {}, {}, {}},
		{"volume", "pc.audio_loudness", "", {}, {}, {}},
	};
	document.Links = {
		{"capture", "audio", "window", "audio_data"},
		{"window", "bit_array", "fft", "data"},
		{"window", "bit_array", "volume", "audio_data"}
	};
	document.Outputs = {
		{"window", "window", "bit_array"}, {"fft", "fft", "array"}, {"volume", "volume", "loudness"}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{
		{"stereo", 2, {}, 10, {{1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2}}}
	};
	EvaluationRequest request;
	request.Tick = 2;
	request.AudioFrames = frames;
	EvaluatedValue value, replay;
	REQUIRE(EvaluateValue(document, plan, "window", request, value, diagnostic) == Status::Ok);
	REQUIRE(std::get<ArrayValue>(value.Data).Nested.size() == 2);
	CHECK(std::get<ArrayValue>(value.Data).Nested.front() == Samples({1, 1, 1, 1}).Elements);
	REQUIRE(EvaluateValue(document, plan, "fft", request, value, diagnostic) == Status::Ok);
	const auto &spectrum = std::get<ArrayValue>(value.Data);
	REQUIRE(spectrum.Nested.size() == 2);
	CHECK(spectrum.Nested.front() == Samples({0, 0, 4}).Elements);
	CHECK(spectrum.Nested.back() == Samples({0, 0, 8}).Elements);
	REQUIRE(EvaluateValue(document, plan, "volume", request, value, diagnostic) == Status::Ok);
	const auto &loudness = std::get<ArrayValue>(value.Data);
	REQUIRE(loudness.Elements.size() == 2);
	CHECK(std::get<double>(loudness.Elements[0]) == 0);
	CHECK(std::abs(std::get<double>(loudness.Elements[1]) - 10 * std::log10(2)) < 1e-12);
	REQUIRE(EvaluateValue(document, plan, "volume", request, replay, diagnostic) == Status::Ok);
	CHECK(replay.Data == value.Data);
	request.Tick = 3;
	CHECK(EvaluateValue(document, plan, "window", request, value, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "capture");
	// A nested source cannot silently appear empty to a legacy flat-array consumer.
	document.Nodes.push_back({"legacy", "image.audio_volume", "", {}, {}, {}});
	document.Links.push_back({"window", "bit_array", "legacy", "samples"});
	document.Outputs.push_back({"legacy", "legacy", "loudness"});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	request.Tick = 2;
	CHECK(
		EvaluateValue(document, plan, "legacy", request, value, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "legacy");
	CHECK(diagnostic.Port == "samples");
	ArrayValue nested{ValueType::Scalar, {}, {{1.0}}};
	document.Nodes.back() = {"legacy", "pc.fft", "", {}, {{"data", nested}}, {}};
	document.Links.pop_back();
	document.Outputs.back() = {"legacy", "legacy", "array"};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
}

TEST_CASE("linked Audio Window integer controls reject out of range scalars", "[imagegraph][node_audio]") {
	for (const std::string &port : std::vector<std::string>{"width", "step"}) {
		for (double number :
			 {1e308, -1e308, 0x1p63, std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity())}) {
			Document document;
			document.FormatVersion = 6;
			document.Nodes = {
				{"number", "value.number", "", {}, {{"value", number}}, {}},
				{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}},
				{"window", "pc.audio_window", "", {}, {{"match_timeline", false}}, {}}
			};
			document.Links = {
				{"number", "number", "window", port}, {"capture", "audio", "window", "audio_data"}
			};
			document.Outputs = {{"window", "window", "bit_array"}};
			Plan plan;
			Diagnostic diagnostic;
			REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
			const std::vector<AudioCaptureFrame> frames{{"mono", 0, {1, 2, 3, 4}, 10}};
			EvaluationRequest request;
			request.AudioFrames = frames;
			EvaluatedValue result;
			CHECK(
				EvaluateValue(document, plan, "window", request, result, diagnostic) == Status::InvalidValue
			);
			CHECK(diagnostic.NodeId == "window");
			CHECK(diagnostic.Port == port);
			CHECK(diagnostic.Message == "integer input must be finite and within int64 range");
		}
	}
	for (double number :
		 {std::numeric_limits<double>::infinity(),
		  -std::numeric_limits<double>::infinity(),
		  std::numeric_limits<double>::quiet_NaN()}) {
		const auto run = RunNode(
			"pc.audio_window",
			{},
			{{"audio_data", AudioBit{{1, 2, 3, 4}, 10}}, {"width", number}, {"match_timeline", false}}
		);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == "width");
	}
	const auto rounded = RunNode(
		"pc.audio_window",
		{},
		{{"audio_data", AudioBit{{1, 2, 3, 4}, 10}},
		 {"width", 2.75},
		 {"step", 1.75},
		 {"cursor_location", .75},
		 {"match_timeline", false}}
	);
	REQUIRE(rounded.Ok);
	CHECK(std::get<ArrayValue>(*rounded.OutputValue("bit_array")).Nested.front() == Samples({1, 3}).Elements);
}

TEST_CASE("Audio Window selects a bounded window from a whole audio clip", "[imagegraph][node_audio]") {
	AudioBit clip;
	clip.SampleRate = 4096;
	clip.Samples.resize(8192);
	for (size_t index = 0; index < clip.Samples.size(); index++)
		clip.Samples[index] = static_cast<double>(index);
	const auto run = RunNode(
		"pc.audio_window",
		{},
		{{"audio_data", clip},
		 {"width", int64_t{4}},
		 {"step", int64_t{1}},
		 {"cursor_location", EnumValue{0}},
		 {"location", .5},
		 {"location_unit", EnumValue{1}},
		 {"match_timeline", false}}
	);
	REQUIRE(run.Ok);
	CHECK(
		std::get<ArrayValue>(*run.OutputValue("bit_array")).Nested.front() ==
		Samples({2048, 2049, 2050, 2051}).Elements
	);
	clip.Samples.resize(Limits::MaximumAudioClipSamples + 1);
	const auto oversized = RunNode("pc.audio_window", {}, {{"audio_data", clip}, {"match_timeline", false}});
	CHECK_FALSE(oversized.Ok);
	CHECK(oversized.Port == "audio_data");
	CHECK(oversized.Values.empty());
}

TEST_CASE(
	"persisted timeline audio drives linked numeric controls from exact captured ticks",
	"[imagegraph][node_audio]"
) {
	Document source;
	source.FormatVersion = 6;
	source.Timeline = TimelineSettings{4, 0, 3, "loop", 4};
	source.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"music"}}}, {}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t{2}},
		  {"step", int64_t{1}},
		  {"cursor_location", EnumValue{0}},
		  {"match_timeline", false}},
		 {}},
		{"volume", "pc.audio_loudness", "", {}, {}, {}},
		{"fft", "pc.fft", "", {}, {}, {}},
		{"control", "pc.number", "", {}, {}, {}}
	};
	source.Links = {
		{"capture", "audio", "window", "audio_data"},
		{"window", "bit_array", "volume", "audio_data"},
		{"window", "bit_array", "fft", "data"},
		{"volume", "loudness", "control", "value"}
	};
	source.Outputs = {{"control", "control", "number"}, {"fft", "fft", "array"}};
	source.Keyframes = {
		{"window", "location", 0, 0.0, "linear", std::nullopt},
		{"window", "location", 2, 2.0, "linear", std::nullopt}
	};
	source.Tracks = {{"window", "location", "loop", -1}};
	Document document;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(source), document, diagnostic) == Status::Ok);
	REQUIRE(document == source);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{
		{"music", 0, {1, 1, 1, 1, 1, 1}, 4},
		{"music", 1, {8, 2, 2, 8, 8, 8}, 4},
		{"music", 2, {}, 4},
		{"music", 5, {4, 4, 4, 4, 4, 4}, 4}
	};
	EvaluationRequest request;
	request.AudioFrames = frames;
	EvaluatedValue value, replay;
	for (const auto &[tick, amplitude] :
		 std::vector<std::pair<uint64_t, double>>{{0, 1}, {5, 4}, {1, 2}, {1, 2}, {2, 0}}) {
		request.Tick = tick;
		REQUIRE(EvaluateValue(document, plan, "control", request, value, diagnostic) == Status::Ok);
		CHECK(std::abs(std::get<double>(value.Data) - (amplitude ? 10 * std::log10(amplitude) : 0)) < 1e-12);
		REQUIRE(EvaluateValue(document, plan, "control", request, replay, diagnostic) == Status::Ok);
		CHECK(replay == value);
		REQUIRE(EvaluateValue(document, plan, "fft", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<ArrayValue>(value.Data) == (amplitude ? Samples({0, 2 * amplitude}) : Samples({})));
	}
	request.Tick = 4;
	CHECK(EvaluateValue(document, plan, "control", request, value, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "capture");
	CHECK(diagnostic.Port == "source_id");
}

TEST_CASE(
	"Audio Window source Int controls round positive and negative halves to even", "[imagegraph][node_audio]"
) {
	const AudioBit audio{{1, 2, 3, 4, 5, 6}, 6};
	for (const auto &[width, count] :
		 std::vector<std::pair<double, size_t>>{{-.5, 0}, {.5, 0}, {1.5, 2}, {2.5, 2}, {3.5, 4}}) {
		const auto run = RunNode(
			"pc.audio_window",
			{},
			{{"audio_data", audio},
			 {"width", width},
			 {"step", int64_t{1}},
			 {"cursor_location", EnumValue{0}},
			 {"match_timeline", false}}
		);
		REQUIRE(run.Ok);
		CHECK(std::get<ArrayValue>(*run.OutputValue("bit_array")).Nested.front().size() == count);
	}
	for (double width : {-1.5, -2.5}) {
		const auto run = RunNode(
			"pc.audio_window", {}, {{"audio_data", audio}, {"width", width}, {"match_timeline", false}}
		);
		CHECK_FALSE(run.Ok);
		CHECK(run.Port == "width");
	}
	for (const auto &[step, expected] : std::vector<std::pair<double, ArrayValue>>{
			 {-2.5, Samples({1, 2, 3, 4})},
			 {-1.5, Samples({1, 2, 3, 4})},
			 {.5, Samples({1, 2, 3, 4})},
			 {1.5, Samples({1, 3})},
			 {2.5, Samples({1, 3})},
			 {3.5, Samples({1})}
		 }) {
		const auto run = RunNode(
			"pc.audio_window",
			{},
			{{"audio_data", audio},
			 {"width", int64_t{4}},
			 {"step", step},
			 {"cursor_location", EnumValue{0}},
			 {"match_timeline", false}}
		);
		REQUIRE(run.Ok);
		CHECK(std::get<ArrayValue>(*run.OutputValue("bit_array")).Nested.front() == expected.Elements);
	}
}

TEST_CASE(
	"Audio Window uses exact real source cursor switches after scalar getter clamping",
	"[imagegraph][node_audio][source_choice]"
) {
	for (const auto &[cursor, expected] : std::vector<std::pair<double, ArrayValue>>{
			 {.5, Samples({1, 2})},
			 {0., Samples({4, 5})},
			 {-5., Samples({4, 5})},
			 {1., Samples({3, 4})},
			 {2., Samples({2, 3})},
			 {99., Samples({2, 3})},
			 {-1e308, Samples({4, 5})},
			 {1e308, Samples({2, 3})}
		 }) {
		CAPTURE(cursor);
		const auto run = RunNode(
			"pc.audio_window",
			{},
			{{"audio_data", AudioBit{{1, 2, 3, 4, 5, 6}, 10}},
			 {"width", int64_t{2}},
			 {"step", int64_t{1}},
			 {"location", 3.},
			 {"cursor_location", cursor},
			 {"match_timeline", false}}
		);
		REQUIRE(run.Ok);
		CHECK(std::get<ArrayValue>(*run.OutputValue("bit_array")).Nested.front() == expected.Elements);
	}
}

TEST_CASE(
	"FFT preserves unknown fractional buffer coercion and clamps verified whole choices",
	"[imagegraph][node_audio][source_choice]"
) {
	const auto data = Samples({.2, -.7, .9, .3, -.1});
	for (const auto &[choice, window] : std::array<std::pair<double, int>, 2>{{{-9., 0}, {99., 2}}}) {
		CAPTURE(choice);
		const auto run = RunNode("pc.fft", {}, {{"data", data}, {"preprocess_function", choice}});
		REQUIRE(run.Ok);
		const auto &actual = std::get<ArrayValue>(*run.OutputValue("array"));
		const auto expected = DirectSpectrum(data, window);
		REQUIRE(actual.Elements.size() == expected.Elements.size());
		for (size_t i = 0; i < actual.Elements.size(); ++i)
			CHECK(
				std::abs(std::get<double>(actual.Elements[i]) - std::get<double>(expected.Elements[i])) < 1e-6
			);
	}
}

TEST_CASE(
	"fractional FFT choice remains explicit until native buffer coercion is known",
	"[imagegraph][node_audio][source_choice]"
) {
	for (double choice : {.5, 1.5}) {
		const auto run =
			RunNode("pc.fft", {}, {{"data", Samples({1, 0, -1, 0})}, {"preprocess_function", choice}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "preprocess_function");
		CHECK(run.Message == "fractional FFT buffer_u32 choice coercion is unverified");
		const auto ignored = RunNode("pc.fft", {}, {{"data", Samples({1})}, {"preprocess_function", choice}});
		REQUIRE(ignored.Ok);
		CHECK(std::get<ArrayValue>(*ignored.OutputValue("array")).Elements.empty());
	}
}

TEST_CASE(
	"nested FFT keeps previous outputs and both plane workspaces in one live budget",
	"[imagegraph][node_audio][allocation_ledger]"
) {
	const auto *entry = FindCatalogueEntry("pc.fft");
	const auto executor = detail::FindExecutor("pc.fft");
	REQUIRE(entry);
	REQUIRE(executor);
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {Samples({1, 1, 1, 1}).Elements, Samples({2, 2, 2, 2}).Elements};
	const Value input = rows;
	const Value window = EnumValue{0};
	const Node authored{"fft", "pc.fft", "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		{
			auto previousCharge = ledger.Reserve(8 * sizeof(double));
			REQUIRE(previousCharge);
			std::vector<double> previous(8, 7);
			{
				detail::NodeContext context(authored, *entry, request, ledger);
				context.ByteBudget = maximum;
				context.ValueViews = {{"data", &input}, {"preprocess_function", &window}};
				CHECK(executor(context) == accepted);
				if (accepted) {
					REQUIRE(context.FailureCode == Status::Ok);
					REQUIRE(context.OutputValues.size() == 1);
					const auto &actual = std::get<ArrayValue>(context.OutputValues.front().Data);
					REQUIRE(actual.Nested.size() == 2);
					CHECK(actual.Nested[0] == Samples({0, 0, 4}).Elements);
					CHECK(actual.Nested[1] == Samples({0, 0, 8}).Elements);
					CHECK(ledger.Peak() >= ledger.Used() + 4 * sizeof(std::complex<double>));
					peak = ledger.Peak();
				} else {
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
				}
				CHECK(previous.front() == 7);
			}
			CHECK(ledger.Used() == previousCharge->Bytes());
		}
		CHECK(ledger.Used() == 0);
	};
	run(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > 0);
	run(peak - 1, false);
}
