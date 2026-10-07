#include <engine/imagegraph/WavPreview.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.wav_preview")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.wav_clip")

using namespace engine::imagegraph;
namespace {
	Document WavDocument() {
		Document document;
		document.FormatVersion = 8;
		document.Nodes.push_back({"wav", "pc.wav_file_read", "", {}, {{"path", std::string("exact.wav")}}});
		document.Outputs.push_back({"data", "wav", "data"});
		return document;
	}
}
TEST_CASE(
	"WAV controls resolve linked animated inputs without materializing a whole clip",
	"[imagegraph][wav_preview]"
) {
	Document document = WavDocument();
	document.Nodes.push_back({"gain", "pc.number", "", {}, {{"value", .25}}});
	document.Links.push_back({"gain", "number", "wav", "attribute_preview_gain"});
	document.Keyframes = {{"gain", "value", 0, .25, "linear"}, {"gain", "value", 10, .75, "step"}};
	std::array sources{
		AudioClipSource{"exact.wav", {std::vector<double>(Limits::MaximumAudioClipSamples, .25), 48000, {}}}
	};
	EvaluationRequest request;
	request.AudioClips = sources;
	request.Tick = 5;
	request.Subframe = .5;
	request.MaximumImageDimension = 128;
	Diagnostic diagnostic;
	WavPreviewControls controls;
	REQUIRE(ResolveWavPreviewControls(document, "wav", request, controls, diagnostic) == Status::Ok);
	CHECK(controls.SourceId == "exact.wav");
	CHECK(controls.Gain == Catch::Approx(.525));
	CHECK(controls.Play);
	CHECK(controls.SampleRate == 48000);
	CHECK(controls.Frames == Limits::MaximumAudioClipSamples);
	CHECK(controls.ShiftSeconds == 0);
	const auto original = controls.SourceId;
	sources[0].SourceId = "Exact.wav";
	CHECK(ResolveWavPreviewControls(document, "wav", request, controls, diagnostic) == Status::InvalidValue);
	CHECK(controls.SourceId == original);
}
TEST_CASE(
	"WAV preview follows pause first selected frame and natural completion order", "[imagegraph][wav_preview]"
) {
	WavPreviewControls controls{"clip", true, .5, .25, 100, 1000};
	WavPreviewIntent intent;
	Diagnostic diagnostic;
	WavPreviewTimeline timeline{true, 10, 10, 10};
	REQUIRE(MakeWavPreviewIntent(controls, timeline, {true, true}, intent, diagnostic) == Status::Ok);
	CHECK(intent.Stop);
	CHECK(intent.Start);
	CHECK(intent.CursorFrames == 75);
	CHECK(intent.Gain == .5);
	timeline.Frame = 11;
	controls.Gain = .75;
	REQUIRE(MakeWavPreviewIntent(controls, timeline, {true, true}, intent, diagnostic) == Status::Ok);
	CHECK_FALSE(intent.Start);
	CHECK_FALSE(intent.Stop);
	REQUIRE(MakeWavPreviewIntent(controls, timeline, {true, false}, intent, diagnostic) == Status::Ok);
	CHECK(intent.Start);
	CHECK_FALSE(intent.Stop);
	CHECK(intent.Gain == .75);
	timeline.Playing = false;
	REQUIRE(MakeWavPreviewIntent(controls, timeline, {true, true}, intent, diagnostic) == Status::Ok);
	CHECK(intent.Stop);
	CHECK_FALSE(intent.Start);
	timeline.Playing = true;
	controls.Play = false;
	REQUIRE(MakeWavPreviewIntent(controls, timeline, {true, true}, intent, diagnostic) == Status::Ok);
	CHECK(intent.Stop);
	CHECK_FALSE(intent.Start);
	CHECK(
		MakeWavPreviewIntent(controls, timeline, {false, true}, intent, diagnostic) == Status::InvalidValue
	);
}
TEST_CASE(
	"WAV preview refuses unsupported offsets atomically while data controls remain independent",
	"[imagegraph][wav_preview]"
) {
	WavPreviewControls controls{"clip", true, .5, 2, 100, 100};
	WavPreviewIntent intent{true, false, 17, .25};
	Diagnostic diagnostic;
	CHECK(
		MakeWavPreviewIntent(controls, {true, 0, 0, 30}, {true, false}, intent, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(intent.CursorFrames == 17);
	controls.ShiftSeconds = 0;
	CHECK(
		MakeWavPreviewIntent(controls, {true, 30, 0, 30}, {true, false}, intent, diagnostic) ==
		Status::UnsupportedExecution
	);
	controls.Frames = 0;
	CHECK(
		MakeWavPreviewIntent(controls, {true, 0, 0, 30}, {true, false}, intent, diagnostic) ==
		Status::UnsupportedExecution
	);
	controls.Gain = std::numeric_limits<double>::infinity();
	CHECK(
		MakeWavPreviewIntent(controls, {true, 0, 0, 30}, {true, false}, intent, diagnostic) ==
		Status::InvalidValue
	);
}
TEST_CASE(
	"WAV preview quantizes original channel zero with source PCM16 attenuation", "[imagegraph][wav_preview]"
) {
	AudioBit clip{{}, 48000, {{1, -1, .5, -0.5, .5 / 16384, 1.5 / 16384}, {0, 0, 0, 0, 0, 0}}};
	Diagnostic diagnostic;
	std::vector<float> samples{17};
	const uint64_t budget = 2 * sizeof(samples) + (samples.capacity() + 6) * sizeof(float);
	REQUIRE(BuildWavPreviewSamples(clip, budget, samples, diagnostic) == Status::Ok);
	CHECK(samples == std::vector<float>{.5f, -.5f, .25f, -.25f, 0, 2.0f / 32768});
	CHECK(BuildWavPreviewSamples(clip, budget - 1, samples, diagnostic) == Status::LimitExceeded);
	CHECK(samples.front() == .5f);
	clip.Channels[0][0] = 2;
	CHECK(
		BuildWavPreviewSamples(clip, Limits::MaximumEvaluationBytes, samples, diagnostic) ==
		Status::UnsupportedExecution
	);
	clip.Channels.clear();
	clip.Samples.clear();
	CHECK(BuildWavPreviewSamples(clip, budget, samples, diagnostic) == Status::UnsupportedExecution);
}
TEST_CASE("WAV sync is a bounded explicit duration action", "[imagegraph][wav_preview]") {
	WavPreviewControls controls{"clip", false, .5, 0, 48000, 48001};
	Diagnostic diagnostic;
	uint64_t frames = 17;
	REQUIRE(WavPreviewSyncFrames(controls, 30, frames, diagnostic) == Status::Ok);
	CHECK(frames == 32);
	controls.Frames = 0;
	REQUIRE(WavPreviewSyncFrames(controls, 30, frames, diagnostic) == Status::Ok);
	CHECK(frames == 1);
	CHECK(WavPreviewSyncFrames(controls, 0, frames, diagnostic) == Status::InvalidValue);
	CHECK(frames == 1);
}

TEST_CASE("Negative paused WAV seeks stop playback before cursor validation", "[imagegraph][wav_preview]") {
	WavPreviewControls controls{"clip", true, .5, 0, 100, 1000};
	WavPreviewIntent intent;
	Diagnostic diagnostic;
	REQUIRE(
		MakeWavPreviewIntent(controls, {false, -.5, 0, 30}, {true, true}, intent, diagnostic) == Status::Ok
	);
	CHECK(intent.Stop);
	CHECK_FALSE(intent.Start);
	const auto retained = intent;
	CHECK(
		MakeWavPreviewIntent(controls, {true, -.5, 0, 30}, {true, false}, intent, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(intent.Stop == retained.Stop);
	CHECK(intent.Start == retained.Start);
}
