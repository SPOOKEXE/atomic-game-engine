#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <studio/WavPreview.hpp>

TEST_SUITE_ID("studio.wav_preview")
TEST_DEPENDS("engine.imagegraph.wav_preview")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.audio.device")
TEST_DEPENDS("engine.audio.mixer")

using namespace engine::imagegraph;
using namespace engine::audio;
namespace {
	Document Graph() {
		Document document;
		document.FormatVersion = 8;
		document.Nodes.push_back({"file", "pc.wav_file_read", "", {}, {{"path", std::string("exact.wav")}}});
		document.Outputs.push_back({"data", "file", "data"});
		return document;
	}
	struct Fixture {
		NullDevice *Device = nullptr;
		studio::ImageGraphWavPreview Preview;
		Document DocumentValue = Graph();
		std::vector<AudioClipSource> Sources{{"exact.wav", {std::vector<double>(64, .5), 8, {}}}};
		studio::ImageGraphPlayback Playback;
		Diagnostic Error;
		uint64_t Revision = 1;
		Fixture()
			: Preview([&](const DeviceSettings &requested) -> std::unique_ptr<engine::audio::Device> {
				  DeviceSettings settings = requested;
				  settings.Format = {8, 2};
				  settings.BlockFrames = 4;
				  auto device = OpenNullDevice(settings);
				  Device = device.get();
				  return device;
			  }) {
			Playback.CurrentTick = 10;
			Playback.StartTick = 10;
			Playback.EndTick = 63;
			Playback.TotalFrames = 64;
			Playback.FramesPerSecond = 8;
			Playback.Playing = true;
		}
		bool Update() {
			return Preview.Update(DocumentValue, Sources, {}, Playback, Revision, 1, Error);
		}
		const engine::audio::Node *AudioPlayer() const {
			return Device->Mixer().Graph().Find({2});
		}
	};
	struct WaveFile {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("studio-wav-preview-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
		explicit WaveFile(uint8_t sample = 192) {
			std::vector<char> bytes;
			auto tag = [&](const char *value) { bytes.insert(bytes.end(), value, value + 4); };
			auto word = [&](uint32_t value, size_t count) {
				for (size_t index = 0; index < count; ++index)
					bytes.push_back(static_cast<char>(value >> (index * 8)));
			};
			tag("RIFF");
			word(100, 4);
			tag("WAVE");
			tag("fmt ");
			word(16, 4);
			word(1, 2);
			word(1, 2);
			word(8, 4);
			word(8, 4);
			word(1, 2);
			word(8, 2);
			tag("data");
			word(64, 4);
			bytes.insert(bytes.end(), 64, static_cast<char>(sample));
			std::ofstream output(Path, std::ios::binary);
			output.write(bytes.data(), bytes.size());
		}
		~WaveFile() {
			std::error_code error;
			std::filesystem::remove(Path, error);
		}
	};
	class PauseFailureDevice final : public engine::audio::Device {
	  public:
		std::unique_ptr<NullDevice> Inner;
		bool RefusePause = false;
		size_t PauseCalls = 0;
		explicit PauseFailureDevice(const DeviceSettings &settings) : Inner(OpenNullDevice(settings)) {}
		AudioMixer &Mixer() override {
			return Inner->Mixer();
		}
		const AudioFormat &Format() const override {
			return Inner->Format();
		}
		bool Running() const override {
			return Inner->Running();
		}
		uint64_t Rendered() const override {
			return Inner->Rendered();
		}
		bool SetPaused(bool paused) override {
			++PauseCalls;
			return RefusePause ? false : Inner->SetPaused(paused);
		}
		bool Paused() const override {
			return Inner->Paused();
		}
		void Close() override {
			Inner->Close();
		}
	};
}
TEST_CASE(
	"WAV owner opens lazily and coalesces the selected first frame until accepted playback",
	"[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device == nullptr);
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	const auto queued = fixture.Device->Mixer().Commands().Pending();
	CHECK(queued == 8);
	for (int pump = 0; pump < 5; ++pump) {
		REQUIRE(fixture.Update());
		CHECK(fixture.Device->Mixer().Commands().Pending() == queued);
	}
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	REQUIRE(fixture.AudioPlayer());
	CHECK(fixture.AudioPlayer()->Playing);
	CHECK(fixture.AudioPlayer()->Cursor == 14);
	CHECK(fixture.Device->LastBlock().Peak() == Catch::Approx(.125));
	for (int pump = 0; pump < 5; ++pump) {
		REQUIRE(fixture.Update());
		CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	}
	fixture.Playback.CurrentTick = 11;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	fixture.Playback.CurrentTick = 10;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 6);
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	fixture.Playback.Playing = false;
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK_FALSE(fixture.AudioPlayer()->Playing);
	fixture.Playback.Playing = true;
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK(fixture.AudioPlayer()->Cursor == 14);
	fixture.Preview.Close();
}
TEST_CASE(
	"WAV owner updates source Gain only on start and restarts natural completion", "[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	fixture.DocumentValue.Nodes[0].Values.push_back({"attribute_preview_gain", .75});
	++fixture.Revision;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	CHECK(fixture.AudioPlayer()->Gain == .5f);
	fixture.Device->Advance(14);
	REQUIRE_FALSE(fixture.AudioPlayer()->Playing);
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 6);
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK(fixture.AudioPlayer()->Gain == .75f);
	CHECK(fixture.AudioPlayer()->Playing);
}
TEST_CASE("WAV owner reserves a whole start burst before queueing any command", "[studio][wav_preview]") {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	Command command;
	for (size_t count = 0; count < CommandQueue::CAPACITY - 7; ++count)
		REQUIRE(fixture.Device->Mixer().Commands().Post(command));
	const auto pending = fixture.Device->Mixer().Commands().Pending();
	CHECK_FALSE(fixture.Update());
	CHECK(fixture.Error.Code == Status::LimitExceeded);
	CHECK(fixture.Device->Mixer().Commands().Pending() == pending);
	fixture.Device->Advance();
	INFO(fixture.Error.Message);
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 8);
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK(fixture.AudioPlayer()->Playing);
}
TEST_CASE(
	"WAV replace stages a fresh sound and removes future rebinds before releasing old pins",
	"[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	std::weak_ptr<const SampleBuffer> previous = fixture.AudioPlayer()->Sound;
	Command future;
	future.Kind = CommandKind::SetSound;
	future.Target = {2};
	future.Sound = fixture.AudioPlayer()->Sound;
	future.PlaybackGeneration = fixture.AudioPlayer()->PlaybackGeneration + 1;
	future.AtSample = 100000;
	REQUIRE(fixture.Device->Mixer().Commands().Post(future));
	future.Sound.reset();
	studio::ImageGraphPreviewCache cache;
	Image image{1, 1, {1, 2, 3, 4}, 0};
	REQUIRE(cache.Store(1, 0, 0, image));
	const auto before = fixture.Sources[0].Data.Samples;
	CHECK_FALSE(fixture.Preview.LoadSource(
		fixture.Sources, cache, "exact.wav", "/tmp/absent-studio-wave.wav", fixture.Error
	));
	CHECK(cache.Find(1, 0, 0));
	CHECK(fixture.Sources[0].Data.Samples == before);
	CHECK_FALSE(previous.expired());
	WaveFile replacement(192);
	REQUIRE(fixture.Preview.LoadSource(fixture.Sources, cache, "exact.wav", replacement.Path, fixture.Error));
	CHECK_FALSE(cache.Find(1, 0, 0));
	CHECK(previous.expired());
	CHECK(fixture.Device->Mixer().Graph().Find({2}) == nullptr);
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	const auto *player = fixture.Device->Mixer().Graph().Find({3});
	REQUIRE(player);
	REQUIRE(fixture.Sources[0].Data.Channels.size() == 1);
	CHECK(fixture.Sources[0].Data.Channels[0][0] == 1.5);
	CHECK(player->Sound->Data()[0] == .75f);
	CHECK(player->PlaybackGeneration > 1);
	std::weak_ptr<const SampleBuffer> replacementPin = player->Sound;
	REQUIRE(fixture.Preview.RemoveSource(fixture.Sources, cache, "exact.wav", fixture.Error));
	CHECK(fixture.Sources.empty());
	CHECK(replacementPin.expired());
}
TEST_CASE(
	"WAV pause failure preserves source cache and pins without retrying the barrier", "[studio][wav_preview]"
) {
	PauseFailureDevice *device = nullptr;
	studio::ImageGraphWavPreview preview(
		[&](const DeviceSettings &settings) -> std::unique_ptr<engine::audio::Device> {
			auto result = std::make_unique<PauseFailureDevice>(settings);
			device = result.get();
			return result;
		}
	);
	Diagnostic diagnostic;
	REQUIRE(preview.Enable(diagnostic));
	Document document = Graph();
	std::vector<AudioClipSource> sources{{"exact.wav", {std::vector<double>(1024, .5), 48000, {}}}};
	studio::ImageGraphPlayback playback;
	playback.Playing = true;
	REQUIRE(preview.Update(document, sources, {}, playback, 1, 1, diagnostic));
	device->Inner->Advance();
	REQUIRE(preview.Update(document, sources, {}, playback, 1, 1, diagnostic));
	const auto *player = device->Mixer().Graph().Find({2});
	REQUIRE(player);
	std::weak_ptr<const SampleBuffer> pin = player->Sound;
	studio::ImageGraphPreviewCache cache;
	Image image{1, 1, {1, 2, 3, 4}, 0};
	REQUIRE(cache.Store(1, 0, 0, image));
	device->RefusePause = true;
	CHECK_FALSE(preview.RemoveSource(sources, cache, "exact.wav", diagnostic));
	CHECK(sources.size() == 1);
	CHECK(cache.Find(1, 0, 0));
	CHECK_FALSE(pin.expired());
	const auto calls = device->PauseCalls;
	CHECK_FALSE(preview.RemoveSource(sources, cache, "exact.wav", diagnostic));
	CHECK(device->PauseCalls == calls);
	preview.Close();
	CHECK(pin.expired());
}
TEST_CASE(
	"WAV Sync length uses resolved source duration and durable timeline history", "[studio][wav_preview]"
) {
	Document document = Graph();
	Document before = document;
	std::array sources{AudioClipSource{"exact.wav", {std::vector<double>(9, .5), 8, {}}}};
	EvaluationRequest request;
	request.AudioClips = sources;
	request.MaximumImageDimension = 128;
	studio::ImageGraphPlayback playback;
	playback.FramesPerSecond = 30;
	Diagnostic diagnostic;
	REQUIRE(studio::SyncImageGraphWavTimeline(document, "file", request, playback, diagnostic));
	REQUIRE(document.Timeline);
	CHECK(document.Timeline->Frames == 35);
	CHECK(document.Timeline->Last == 34);
	studio::ImageGraphHistory history;
	history.Record(before, document);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.Redo(document));
	CHECK(document == restored);
	sources[0].SourceId = "missing.wav";
	CHECK_FALSE(studio::SyncImageGraphWavTimeline(document, "file", request, playback, diagnostic));
	CHECK(document == restored);
}

TEST_CASE(
	"WAV owner ignores stale copied events and rebuilds a newer voice after event loss",
	"[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	const auto generation = fixture.AudioPlayer()->PlaybackGeneration;
	Command command;
	command.Kind = CommandKind::Stop;
	command.Target = {2};
	command.PlaybackGeneration = generation + 10;
	REQUIRE(fixture.Device->Mixer().Commands().Post(command));
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK(fixture.AudioPlayer()->Playing);
	CHECK(fixture.AudioPlayer()->PlaybackGeneration == generation);
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	command.Kind = CommandKind::Rewind;
	command.PlaybackGeneration = generation;
	for (size_t batch = 0; batch < 3; ++batch) {
		for (size_t index = 0; index < 700; ++index)
			REQUIRE(fixture.Device->Mixer().Commands().Post(command));
		fixture.Device->Advance();
	}
	REQUIRE(fixture.Device->Mixer().PlaybackEventsDropped() > 0);
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Graph().Find({2}) == nullptr);
	CHECK(fixture.Device->Mixer().Commands().Pending() == 8);
	// The new generation remains pending until the whole acknowledgement batch arrives.
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 8);
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	const auto *player = fixture.Device->Mixer().Graph().Find({3});
	REQUIRE(player);
	CHECK(player->PlaybackGeneration > generation);
	CHECK(player->Playing);
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
}

TEST_CASE(
	"WAV linked image controls share the native preview cap in playback and Sync", "[studio][wav_preview]"
) {
	Fixture fixture;
	fixture.DocumentValue.Nodes.push_back(
		{"fill", "pc.solid", "", {}, {{"dimension", Vector2{128, 1}}, {"dimension_unit", EnumValue{0}}}}
	);
	fixture.DocumentValue.Nodes.push_back({"surface", "pc.surface_data", "", {}, {}});
	fixture.DocumentValue.Nodes.push_back({"gain", "pc.to_number", "", {}, {}});
	fixture.DocumentValue.Links = {
		{"fill", "surface_out", "surface", "surface"},
		{"surface", "format_string", "gain", "text"},
		{"gain", "number", "file", "attribute_preview_gain"}
	};
	EvaluationRequest request;
	request.AudioClips = fixture.Sources;
	REQUIRE(
		studio::SyncImageGraphWavTimeline(
			fixture.DocumentValue, "file", request, fixture.Playback, fixture.Error
		)
	);
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	REQUIRE(fixture.AudioPlayer());
	CHECK(fixture.AudioPlayer()->Gain == 8.f);
	REQUIRE(
		studio::SetImageGraphValue(fixture.DocumentValue, "fill", "dimension", Vector2{129, 1}, fixture.Error)
	);
	++fixture.Revision;
	const Document before = fixture.DocumentValue;
	CHECK_FALSE(
		studio::SyncImageGraphWavTimeline(
			fixture.DocumentValue, "file", request, fixture.Playback, fixture.Error
		)
	);
	CHECK(fixture.Error.Code == Status::LimitExceeded);
	CHECK(fixture.Error.NodeId == "fill");
	CHECK(fixture.DocumentValue == before);
	CHECK_FALSE(fixture.Update());
	CHECK(fixture.Error.Code == Status::LimitExceeded);
	CHECK(fixture.Error.NodeId == "fill");
	request.MaximumImageDimension = 0;
	CHECK_FALSE(
		studio::SyncImageGraphWavTimeline(
			fixture.DocumentValue, "file", request, fixture.Playback, fixture.Error
		)
	);
	CHECK(fixture.Error.Code == Status::InvalidValue);
	request.MaximumImageDimension = Limits::MaximumDimension + 1;
	CHECK_FALSE(
		studio::SyncImageGraphWavTimeline(
			fixture.DocumentValue, "file", request, fixture.Playback, fixture.Error
		)
	);
	CHECK(fixture.Error.Code == Status::InvalidValue);
	CHECK(fixture.DocumentValue == before);
}

TEST_CASE("WAV owner keeps lazy opening errors until a successful explicit enable", "[studio][wav_preview]") {
	bool available = false;
	size_t opens = 0;
	studio::ImageGraphWavPreview preview(
		[&](const DeviceSettings &settings) -> std::unique_ptr<engine::audio::Device> {
			++opens;
			return available ? OpenNullDevice(settings) : nullptr;
		}
	);
	Diagnostic diagnostic;
	CHECK_FALSE(preview.Enabled());
	CHECK_FALSE(preview.Enable(diagnostic));
	CHECK_FALSE(preview.Enabled());
	CHECK(diagnostic.Code == Status::UnsupportedExecution);
	Document document = Graph();
	studio::ImageGraphPlayback playback;
	REQUIRE(preview.Update(document, {}, {}, playback, 1, 1, diagnostic));
	CHECK_FALSE(preview.Enabled());
	CHECK(opens == 1);
	available = true;
	REQUIRE(preview.Enable(diagnostic));
	CHECK(preview.Enabled());
	CHECK(diagnostic.Code == Status::Ok);
	CHECK(opens == 2);
	preview.Close();
	CHECK_FALSE(preview.Enabled());
}

TEST_CASE(
	"WAV matching requested generation refusal recovers instead of leaving a pending stop",
	"[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	Command replacement;
	replacement.Kind = CommandKind::SetSound;
	replacement.Target = {2};
	replacement.Sound = fixture.AudioPlayer()->Sound;
	replacement.PlaybackGeneration = fixture.AudioPlayer()->PlaybackGeneration + 10;
	REQUIRE(fixture.Device->Mixer().Commands().Post(replacement));
	replacement.Sound.reset();
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	fixture.DocumentValue.Nodes[0].Values.push_back({"attribute_play", false});
	++fixture.Revision;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 1);
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Graph().Find({2}) == nullptr);
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	REQUIRE(studio::SetImageGraphValue(fixture.DocumentValue, "file", "attribute_play", true, fixture.Error));
	++fixture.Revision;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 8);
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	const auto *player = fixture.Device->Mixer().Graph().Find({3});
	REQUIRE(player);
	CHECK(player->PlaybackGeneration > replacement.PlaybackGeneration);
	CHECK(player->Playing);
}

TEST_CASE(
	"WAV fresh paused or disabled controls never create a player or consume queue space",
	"[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	fixture.Playback.Playing = false;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	CHECK(fixture.AudioPlayer() == nullptr);
	fixture.Playback.Playing = true;
	fixture.DocumentValue.Nodes[0].Values.push_back({"attribute_play", false});
	++fixture.Revision;
	REQUIRE(fixture.Update());
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	CHECK(fixture.AudioPlayer() == nullptr);
}

TEST_CASE("WAV preparation reserves metadata as well as both float payloads", "[studio][wav_preview]") {
	Fixture fixture;
	fixture.Sources[0].Data.Samples.assign(Limits::MaximumAudioClipSamples, .5);
	for (size_t index = 1; index < 15; ++index)
		fixture.Sources.push_back(
			{"retained-" + std::to_string(index),
			 {std::vector<double>(Limits::MaximumAudioClipSamples, .25), 8, {}}}
		);
	uint64_t retained = 0;
	for (const auto &source : fixture.Sources)
		retained +=
			sizeof(AudioClipSource) + source.SourceId.size() + source.Data.Samples.size() * sizeof(double);
	const uint64_t floatPayloads = 2 * Limits::MaximumAudioClipSamples * sizeof(float);
	// Leave room for conversion payloads and their vector, but not a new retained sound record.
	const uint64_t excess =
		retained + floatPayloads + sizeof(std::vector<float>) - Limits::MaximumEvaluationBytes;
	const size_t remove = static_cast<size_t>((excess + sizeof(double) - 1) / sizeof(double));
	REQUIRE(remove < fixture.Sources.back().Data.Samples.size());
	fixture.Sources.back().Data.Samples.resize(fixture.Sources.back().Data.Samples.size() - remove);
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	CHECK_FALSE(fixture.Update());
	CHECK(fixture.Error.Code == Status::LimitExceeded);
	CHECK(fixture.Device->Mixer().Commands().Pending() == 0);
	CHECK(fixture.AudioPlayer() == nullptr);
	CHECK(fixture.Sources[0].Data.Samples.size() == Limits::MaximumAudioClipSamples);
}

TEST_CASE(
	"WAV replacement retains a heap source name borrowed from the replaced source", "[studio][wav_preview]"
) {
	Fixture fixture;
	const std::string durableId = std::string(16384, 'w') + ".wav";
	fixture.Sources[0].SourceId = durableId;
	fixture.Sources[0].Data.Samples.assign(64, .25);
	fixture.Sources.push_back({"next.wav", {{.5}, 8, {}}});
	fixture.Sources.push_back({"last.wav", {{.75}, 8, {}}});
	REQUIRE(studio::SetImageGraphValue(fixture.DocumentValue, "file", "path", durableId, fixture.Error));
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	std::weak_ptr<const SampleBuffer> oldPin = fixture.AudioPlayer()->Sound;
	studio::ImageGraphPreviewCache cache;
	WaveFile replacement(64);
	const std::string_view borrowedId = fixture.Sources[0].SourceId;
	REQUIRE(fixture.Preview.LoadSource(fixture.Sources, cache, borrowedId, replacement.Path, fixture.Error));
	CHECK(fixture.Sources[0].SourceId == durableId);
	CHECK(oldPin.expired());
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	const auto *player = fixture.Device->Mixer().Graph().Find({3});
	REQUIRE(player);
	CHECK(player->Sound->Data()[0] == .25f);
	CHECK(player->Playing);
	std::weak_ptr<const SampleBuffer> newPin = player->Sound;
	REQUIRE(fixture.Preview.RemoveSource(fixture.Sources, cache, fixture.Sources[0].SourceId, fixture.Error));
	REQUIRE(fixture.Sources.size() == 2);
	CHECK(fixture.Sources[0].SourceId == "next.wav");
	CHECK(fixture.Sources[0].Data.Samples == std::vector<double>{.5});
	CHECK(fixture.Sources[1].SourceId == "last.wav");
	CHECK(fixture.Sources[1].Data.Samples == std::vector<double>{.75});
	CHECK(newPin.expired());
}

TEST_CASE(
	"WAV host stops a retained player when the author cursor seeks a negative fraction",
	"[studio][wav_preview]"
) {
	Fixture fixture;
	REQUIRE(fixture.Preview.Enable(fixture.Error));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	REQUIRE(fixture.AudioPlayer());
	CHECK(fixture.AudioPlayer()->Playing);
	fixture.Playback.Playing = false;
	REQUIRE(studio::SeekImageGraphAuthorFrame(fixture.Playback, -1.25, true, true));
	REQUIRE(fixture.Update());
	fixture.Device->Advance();
	REQUIRE(fixture.Update());
	CHECK_FALSE(fixture.AudioPlayer()->Playing);
	CHECK(studio::GetImageGraphFrame(fixture.Playback) == FrameTime{1, .25, true});
}
