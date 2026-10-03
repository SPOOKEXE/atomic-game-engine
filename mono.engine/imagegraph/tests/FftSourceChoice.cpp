#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/WavClip.hpp>
#include <engine/imagegraph/WavExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <span>
#include <tuple>

TEST_SUITE_ID("engine.imagegraph.fft_source_choice")
using namespace engine::imagegraph;
namespace {
	ArrayValue Spectrum(std::span<const double> data, int window) {
		ArrayValue result{ValueType::Scalar, {}};
		size_t padded = 1;
		while (padded < data.size())
			padded *= 2;
		for (size_t bin = padded / 2 + 1; bin > 0; --bin) {
			std::complex<double> sum{};
			for (size_t i = 0; i < data.size(); ++i) {
				const double phase = 2 * std::numbers::pi * i / data.size();
				const double gain = window == 1	  ? .5 * (1 - std::cos(phase))
									: window == 2 ? .42 - .5 * std::cos(phase) + .08 * std::cos(2 * phase)
												  : 1;
				const double angle = -2 * std::numbers::pi * (bin - 1) * i / padded;
				sum += data[i] * gain * std::complex<double>{std::cos(angle), std::sin(angle)};
			}
			result.Elements.emplace_back(double(float(std::abs(sum))));
		}
		return result;
	}
	void Compare(const std::vector<ElementValue> &actual, const ArrayValue &expected) {
		REQUIRE(actual.size() == expected.Elements.size());
		for (size_t i = 0; i < actual.size(); ++i)
			CHECK(std::abs(std::get<double>(actual[i]) - std::get<double>(expected.Elements[i])) < 1e-6);
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{4, 0, 3, "loop", 4};
		document.Nodes = {
			{"file", "pc.wav_file_read", "", {}, {{"path", std::string{"fixture.wav"}}}},
			{"window",
			 "pc.audio_window",
			 "",
			 {},
			 {{"width", int64_t{4}}, {"step", int64_t{1}}, {"cursor_location", EnumValue{0}}}},
			{"choice", "value.number", "", {}, {{"value", .5}}},
			{"fft", "pc.fft", "", {}, {}}
		};
		document.Keyframes = {{"choice", "value", 0, .5, "linear"}, {"choice", "value", 3, 2., "linear"}};
		document.Links = {
			{"file", "data", "window", "audio_data"},
			{"window", "bit_array", "fft", "data"},
			{"choice", "number", "fft", "preprocess_function"}
		};
		document.Outputs = {{"spectrum", "fft", "array"}};
		return document;
	}
	AudioClipSource Clip() {
		ArrayValue channels{ValueType::Scalar, {}};
		channels.Nested.resize(2);
		for (size_t i = 0; i < 16; ++i) {
			channels.Nested[0].emplace_back(double(int(i % 4) * 4000 - 6000));
			channels.Nested[1].emplace_back(double(int(i % 3) * 6000 - 5000));
		}
		Diagnostic diagnostic;
		std::vector<std::byte> bytes;
		REQUIRE(
			EncodeWavExport(channels, {8, WavExportFormat::Signed16}, 4096, bytes, diagnostic) == Status::Ok
		);
		AudioBit audio;
		REQUIRE(DecodeWavClip(bytes, WavClipPolicy::PixelComposer, 4096, audio, diagnostic) == Status::Ok);
		REQUIRE(audio.Channels.size() == 2);
		REQUIRE(audio.Channels.front().size() == 16);
		return {"fixture.wav", std::move(audio)};
	}
}

TEST_CASE(
	"real WAV windows feed linked fractional FFT controls across signed seeks",
	"[imagegraph][fft_source_choice]"
) {
	const auto document = Graph();
	const auto clip = Clip();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.AudioClips = std::span(&clip, 1);
	for (const auto &[tick, fraction, negative, start, window] :
		 std::array<std::tuple<uint64_t, double, bool, size_t, int>, 6>{
			 {{1, .5, false, 3, 1},
			  {0, 0, false, 0, 0},
			  {2, .5, false, 5, 1},
			  {3, 0, false, 6, 2},
			  {1, .5, false, 3, 1},
			  {0, .5, true, 0, 0}}
		 }) {
		request.Tick = tick;
		request.Subframe = fraction;
		request.NegativeFrame = negative;
		const bool ready =
			EvaluateStateful(document, plan, "spectrum", request, result, diagnostic) == Status::Ok;
		INFO(diagnostic.Message);
		REQUIRE(ready);
		const auto &output = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Output).Data);
		REQUIRE(output.Nested.size() == 2);
		for (size_t channel = 0; channel < 2; ++channel)
			Compare(
				output.Nested[channel],
				Spectrum(std::span(clip.Data.Channels[channel]).subspan(start, 4), window)
			);
	}
	const auto retained = std::get<EvaluatedValue>(result.Output);
	request.Tick = 2;
	request.Subframe = .25;
	request.NegativeFrame = false;
	CHECK(
		EvaluateStateful(document, plan, "spectrum", request, result, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(std::get<EvaluatedValue>(result.Output) == retained);
	request.AudioClips = {};
	CHECK(EvaluateStateful(document, plan, "spectrum", request, result, diagnostic) == Status::InvalidValue);
	CHECK(std::get<EvaluatedValue>(result.Output) == retained);
	request.AudioClips = std::span(&clip, 1);
	REQUIRE(EvaluateStateful(document, plan, "spectrum", request, result, diagnostic) == Status::Ok);
}

TEST_CASE(
	"FFT processor rows apply unsigned modulo after source array clamp bypass",
	"[imagegraph][fft_source_choice]"
) {
	auto document = Graph();
	document.Keyframes.clear();
	document.Nodes[2] = {
		"choice",
		"pc.number",
		"",
		{},
		{{"value",
		  ArrayValue{
			  ValueType::Scalar,
			  {-1.5, .5, 1.5, 4294967296.5, 4294967297.5, std::numeric_limits<double>::max()}
		  }}}
	};
	const auto clip = Clip();
	EvaluationRequest request;
	request.AudioClips = std::span(&clip, 1);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	const auto status = EvaluateValue(document, plan, "spectrum", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	// Source processor rows loop the two audio channels against the six selected controls.
	const auto &rows = std::get<ArrayValue>(output.Data);
	REQUIRE(rows.Nested.size() == 6);
	const std::array<int, 6> windows{0, 0, 1, 0, 1, 0};
	for (size_t row = 0; row < rows.Nested.size(); ++row)
		Compare(rows.Nested[row], Spectrum(std::span(clip.Data.Channels[row % 2]).first(4), windows[row]));
}

TEST_CASE(
	"nonfinite FFT controls refuse before replacing the compiled plan or output",
	"[imagegraph][fft_source_choice]"
) {
	const auto document = Graph();
	const auto clip = Clip();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const Plan retainedPlan = plan;
	EvaluationRequest request;
	request.AudioClips = std::span(&clip, 1);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "spectrum", request, result, diagnostic) == Status::Ok);
	const auto retained = result;
	for (double choice :
		 {std::numeric_limits<double>::quiet_NaN(),
		  std::numeric_limits<double>::infinity(),
		  -std::numeric_limits<double>::infinity()}) {
		auto invalid = document;
		invalid.Nodes[2].Values.front().Data = choice;
		CHECK(Compile(invalid, plan, diagnostic) != Status::Ok);
		CHECK(plan == retainedPlan);
		CHECK(result == retained);
	}
	REQUIRE(EvaluateValue(document, plan, "spectrum", request, result, diagnostic) == Status::Ok);
	CHECK(result == retained);
}
