#include "NodeExecutors.hpp"

#include <engine/imagegraph/WavClip.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.wav_clip")
TEST_DEPENDS("engine.audio.wav")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;
	std::vector<std::byte>
	Wave(uint16_t encoding, uint16_t bits, uint16_t channels, std::span<const uint8_t> data) {
		std::vector<std::byte> bytes;
		const auto word = [&](uint32_t value, size_t count) {
			for (size_t index = 0; index < count; index++)
				bytes.push_back(std::byte((value >> (index * 8)) & 255));
		};
		const auto tag = [&](const char *text) {
			for (size_t index = 0; index < 4; index++)
				bytes.push_back(std::byte(text[index]));
		};
		tag("RIFF");
		word(36 + data.size() + data.size() % 2, 4);
		tag("WAVE");
		tag("fmt ");
		word(16, 4);
		word(encoding, 2);
		word(channels, 2);
		word(8, 4);
		word(8 * channels * bits / 8, 4);
		word(channels * bits / 8, 2);
		word(bits, 2);
		tag("data");
		word(data.size(), 4);
		for (uint8_t value : data)
			bytes.push_back(std::byte(value));
		if (data.size() % 2) bytes.push_back(std::byte{0});
		return bytes;
	}

	Document FileDocument() {
		Document document;
		// Authored source choices use the v6 enum grammar.
		document.FormatVersion = 6;
		document.Nodes.push_back(
			{"file", "pc.wav_file_read", "", {}, {{"path", std::string("tone.wav")}}, {}}
		);
		for (const std::string port : {"data", "path", "sample_rate", "channels", "duration"})
			document.Outputs.push_back({port, "file", port});
		return document;
	}
}

TEST_CASE("WAV clip policies retain known sample bytes and channel planes", "[imagegraph][wav_clip]") {
	Diagnostic diagnostic;
	AudioBit clip;
	const std::array<uint8_t, 8> stereo16{0, 128, 255, 127, 0, 64, 0, 192};
	const auto wav = Wave(1, 16, 2, stereo16);
	REQUIRE(DecodeWavClip(wav, WavClipPolicy::PixelComposer, 4096, clip, diagnostic) == Status::Ok);
	REQUIRE(clip.SampleRate == 8);
	REQUIRE(clip.Samples.empty());
	REQUIRE(clip.Channels == std::vector<std::vector<double>>{{-1, 0.5}, {32767.0 / 32768, -0.5}});
	REQUIRE(DecodeWavClip(wav, WavClipPolicy::Native, 4096, clip, diagnostic) == Status::Ok);
	REQUIRE(clip.Channels[0] == std::vector<double>{-1, 0.5});
	const std::array<uint8_t, 3> pcm8{0, 128, 255};
	REQUIRE(
		DecodeWavClip(Wave(1, 8, 1, pcm8), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) == Status::Ok
	);
	REQUIRE(clip.Channels[0] == std::vector<double>{0, 1, 255.0 / 128});
	REQUIRE(DecodeWavClip(Wave(1, 8, 1, pcm8), WavClipPolicy::Native, 4096, clip, diagnostic) == Status::Ok);
	REQUIRE(clip.Channels[0] == std::vector<double>{-1, 0, 127.0 / 128});
	const std::array<uint8_t, 8> pcm32{1, 0, 0, 0, 255, 255, 255, 127};
	REQUIRE(
		DecodeWavClip(Wave(1, 32, 1, pcm32), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) ==
		Status::Ok
	);
	REQUIRE(clip.Channels[0] == std::vector<double>{1.0 / 2147483648, 2147483647.0 / 2147483648});
	REQUIRE(
		DecodeWavClip(Wave(1, 32, 1, pcm32), WavClipPolicy::Native, 4096, clip, diagnostic) ==
		Status::InvalidValue
	);
	const std::array<uint8_t, 8> float32{0, 0, 0, 63, 0, 0, 0, 191};
	REQUIRE(
		DecodeWavClip(Wave(3, 32, 2, float32), WavClipPolicy::Native, 4096, clip, diagnostic) == Status::Ok
	);
	REQUIRE(clip.Channels == std::vector<std::vector<double>>{{0.5}, {-0.5}});
	REQUIRE(
		DecodeWavClip(Wave(3, 32, 2, float32), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) ==
		Status::InvalidValue
	);
	const std::array<uint8_t, 6> pcm24{0, 0, 128, 0, 0, 64};
	REQUIRE(
		DecodeWavClip(Wave(1, 24, 1, pcm24), WavClipPolicy::Native, 4096, clip, diagnostic) == Status::Ok
	);
	REQUIRE(clip.Channels[0] == std::vector<double>{-1, 0.5});
	REQUIRE(
		DecodeWavClip(Wave(1, 24, 1, pcm24), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) ==
		Status::InvalidValue
	);
}

TEST_CASE("WAV clip refusals are bounded and atomic", "[imagegraph][wav_clip]") {
	AudioBit clip{{0.25}, 8, {}};
	const AudioBit previous = clip;
	Diagnostic diagnostic;
	const std::array<uint8_t, 2> samples{0, 128};
	const auto valid = Wave(1, 8, 1, samples);
	REQUIRE(DecodeWavClip(valid, WavClipPolicy::PixelComposer, 1, clip, diagnostic) == Status::LimitExceeded);
	REQUIRE(clip == previous);
	REQUIRE(
		DecodeWavClip(std::span(valid).first(20), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) ==
		Status::InvalidValue
	);
	REQUIRE(clip == previous);
	const std::array<uint8_t, 4> infinity{0, 0, 128, 127};
	REQUIRE(
		DecodeWavClip(Wave(3, 32, 1, infinity), WavClipPolicy::Native, 4096, clip, diagnostic) ==
		Status::InvalidValue
	);
	REQUIRE(clip == previous);
	const std::vector<uint8_t> excess(Limits::MaximumAudioClipSamples + 1);
	REQUIRE(
		DecodeWavClip(
			Wave(1, 8, 1, excess),
			WavClipPolicy::PixelComposer,
			Limits::MaximumEvaluationBytes,
			clip,
			diagnostic
		) == Status::LimitExceeded
	);
	REQUIRE(clip == previous);
	REQUIRE(
		DecodeWavClip(
			Wave(1, 8, 9, std::array<uint8_t, 9>{}), WavClipPolicy::PixelComposer, 4096, clip, diagnostic
		) == Status::InvalidValue
	);
	REQUIRE(clip == previous);
}

TEST_CASE("Whole WAV assets route metadata mono data and Window FFT", "[imagegraph][wav_clip]") {
	const std::array<uint8_t, 20> samples{0, 0, 0, 0, 0, 64, 0, 192, 0, 0, 0, 0, 0, 192, 0, 64, 0, 0, 0, 0};
	AudioBit clip;
	Diagnostic diagnostic;
	REQUIRE(
		DecodeWavClip(Wave(1, 16, 2, samples), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) ==
		Status::Ok
	);
	std::array assets{AudioClipSource{"tone.wav", clip}};
	EvaluationRequest request;
	request.Tick = 100;
	request.AudioClips = assets;
	Document document = FileDocument();
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "channels", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<int64_t>(result.Data) == 2);
	REQUIRE(EvaluateValue(document, plan, "duration", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<double>(result.Data) == 0.625);
	REQUIRE(EvaluateValue(document, plan, "sample_rate", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<int64_t>(result.Data) == 8);
	REQUIRE(EvaluateValue(document, plan, "path", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<std::string>(result.Data) == "tone.wav");
	document.Nodes.front().Values.push_back({"mono", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "data", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<AudioBit>(result.Data).Channels == std::vector<std::vector<double>>{{0, 0, 0, 0, 0}});
	REQUIRE(EvaluateValue(document, plan, "channels", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<int64_t>(result.Data) == 2);
	document.Nodes.front().Values.pop_back();
	document.Nodes.push_back(
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t(4)},
		  {"step", int64_t(1)},
		  {"cursor_location", EnumValue{0}},
		  {"match_timeline", false}},
		 {}}
	);
	document.Nodes.push_back({"fft", "pc.fft", "", {}, {{"preprocess_function", EnumValue{0}}}, {}});
	document.Links.push_back({"file", "data", "window", "audio_data"});
	document.Links.push_back({"window", "bit_array", "fft", "data"});
	document.Outputs.push_back({"spectrum", "fft", "array"});
	const Status compiledStatus = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compiledStatus == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "spectrum", request, result, diagnostic) == Status::Ok);
	const auto &spectrum = std::get<ArrayValue>(result.Data);
	REQUIRE(spectrum.Nested.size() == 2);
	for (const auto &channel : spectrum.Nested) {
		REQUIRE(channel.size() == 3);
		REQUIRE(std::get<double>(channel[0]) == Catch::Approx(0));
		REQUIRE(std::get<double>(channel[1]) == Catch::Approx(1));
		REQUIRE(std::get<double>(channel[2]) == Catch::Approx(0));
	}
}

TEST_CASE(
	"WAV source rejects invalid clips while playback controls preserve data", "[imagegraph][wav_clip]"
) {
	Document document = FileDocument();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value{"sentinel", 0.25};
	EvaluationRequest request;
	REQUIRE(EvaluateValue(document, plan, "data", request, value, diagnostic) == Status::InvalidValue);
	REQUIRE(value.Port == "sentinel");
	std::array assets{
		AudioClipSource{"tone.wav", {{0.5}, 8, {}}}, AudioClipSource{"tone.wav", {{0.25}, 8, {}}}
	};
	request.AudioClips = assets;
	REQUIRE(EvaluateValue(document, plan, "data", request, value, diagnostic) == Status::DuplicateId);
	request.AudioClips = std::span(assets).first(1);
	assets[0].Data.SampleRate = 1e100;
	REQUIRE(EvaluateValue(document, plan, "data", request, value, diagnostic) == Status::InvalidValue);
	assets[0].Data.SampleRate = 8;
	const auto *entry = FindCatalogueEntry("pc.wav_file_read");
	REQUIRE(entry);
	detail::NodeContext context(document.Nodes.front(), *entry, request);
	context.Values.push_back({"path", std::string("tone.wav")});
	context.ByteBudget = 1;
	const auto executor = detail::FindExecutor("pc.wav_file_read");
	REQUIRE(executor);
	REQUIRE_FALSE(executor(context));
	REQUIRE(context.FailureCode == Status::LimitExceeded);
	REQUIRE(context.OutputValues.empty());
	REQUIRE(EvaluateValue(document, plan, "data", request, value, diagnostic) == Status::Ok);
	const AudioBit unchanged = std::get<AudioBit>(value.Data);
	for (AuthoredValue control :
		 {AuthoredValue{"sync_length", true},
		  AuthoredValue{"attribute_play", false},
		  AuthoredValue{"attribute_preview_gain", 0.75},
		  AuthoredValue{"attribute_preview_shift", 1.0}}) {
		document.Nodes.front().Values.push_back(control);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(document, plan, "data", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<AudioBit>(value.Data) == unchanged);
		document.Nodes.front().Values.pop_back();
	}
}

TEST_CASE("WAV whole clips exceed capture windows without truncation", "[imagegraph][wav_clip]") {
	std::vector<uint8_t> samples(5000, 0);
	samples[4096] = 128;
	AudioBit clip;
	Diagnostic diagnostic;
	REQUIRE(
		DecodeWavClip(Wave(1, 8, 1, samples), WavClipPolicy::PixelComposer, 100000, clip, diagnostic) ==
		Status::Ok
	);
	REQUIRE(clip.Channels.front().size() == 5000);
	std::array assets{AudioClipSource{"tone.wav", std::move(clip)}};
	EvaluationRequest request;
	request.AudioClips = assets;
	Document document = FileDocument();
	document.Nodes.push_back(
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t(4)},
		  {"step", int64_t(1)},
		  {"cursor_location", EnumValue{0}},
		  {"match_timeline", false},
		  {"location", 4096.0}},
		 {}}
	);
	document.Links.push_back({"file", "data", "window", "audio_data"});
	document.Outputs.push_back({"late", "window", "bit_array"});
	Plan plan;
	const Status compiledStatus = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(compiledStatus == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "duration", request, result, diagnostic) == Status::Ok);
	REQUIRE(std::get<double>(result.Data) == 625);
	REQUIRE(EvaluateValue(document, plan, "late", request, result, diagnostic) == Status::Ok);
	const auto &row = std::get<ArrayValue>(result.Data).Nested.front();
	REQUIRE(row.size() == 4);
	REQUIRE(std::get<double>(row.front()) == 1);
	REQUIRE(std::get<double>(row.back()) == 0);
}

TEST_CASE(
	"WAV output cloning admits the retained clip and long path beside previous live data",
	"[imagegraph][wav_clip][allocation_ledger]"
) {
	using namespace engine::imagegraph;
	const auto *entry = FindCatalogueEntry("pc.wav_file_read");
	const auto executor = detail::FindExecutor("pc.wav_file_read");
	REQUIRE(entry);
	REQUIRE(executor);
	const std::string path(128, 'w');
	AudioBit audio;
	audio.SampleRate = 8;
	audio.Channels = {{1, 2, 3, 4}, {5, 6, 7, 8}};
	const std::array<AudioClipSource, 1> assets{{{path, audio}}};
	EvaluationRequest request;
	request.AudioClips = assets;
	const Value pathValue = path;
	const Value mono = false;
	const Node authored{"file", "pc.wav_file_read", "", {}, {}};
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		{
			auto previousCharge = ledger.Reserve(8 * sizeof(double));
			REQUIRE(previousCharge);
			std::vector<double> previous(8, 9);
			{
				detail::NodeContext context(authored, *entry, request, ledger);
				context.ByteBudget = maximum;
				context.ValueViews = {{"path", &pathValue}, {"mono", &mono}};
				CHECK(executor(context) == accepted);
				if (accepted) {
					REQUIRE(context.FailureCode == Status::Ok);
					REQUIRE(context.OutputValues.size() == 5);
					const auto &actual = std::get<AudioBit>(context.OutputValues[0].Data);
					CHECK(actual.Channels == audio.Channels);
					CHECK(actual.SampleRate == audio.SampleRate);
					CHECK(std::get<std::string>(context.OutputValues[1].Data) == path);
					CHECK(ledger.Peak() == ledger.Used());
					peak = ledger.Peak();
				} else {
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
				}
				CHECK(previous.back() == 9);
			}
			CHECK(ledger.Used() == previousCharge->Bytes());
		}
		CHECK(ledger.Used() == 0);
	};
	run(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > 0);
	run(peak - 1, false);
}
