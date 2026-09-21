#include <engine/ecs/Store.hpp>
#include <engine/script/DataAudioObservationBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>

TEST_SUITE_ID("engine.script.dataaudioobservationbridge")

namespace {
	constexpr const char *DIGEST = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

	engine::script::DataAudioObservation ValidObservation() {
		using namespace engine::script;
		DataAudioObservation observation;
		observation.ObservationId = "audio/1";
		observation.World = {
			.Tick = 7,
			.TimeNanoseconds = 116'666'666,
			.Epoch = 2,
			.Version = "world/v1",
			.TickInterval = {.NumeratorNanoseconds = 1'000'000'000, .Denominator = 60}
		};
		observation.SampleBegin = 100;
		observation.SampleEnd = 101;
		observation.SampleRateHz = 48'000;
		observation.Channels = 2;
		observation.ChannelLayout = "stereo";
		observation.Waveform = {
			.Available = true,
			.UnavailableReason = {},
			.ResourceId = "audio/1/waveform",
			.Sha256 = DIGEST,
			.ByteLength = 8,
			.Chunks = {{.Sha256 = DIGEST, .ByteBegin = 0, .ByteEnd = 8}}
		};
		observation.Sources = {
			{.SourceId = "source/a",
			 .Kind = "player",
			 .Gain = 0.5,
			 .Pan = -0.25,
			 .Playing = true,
			 .Present = true}
		};
		observation.Events = {
			{.EventId = "event/a",
			 .Kind = "play",
			 .SourceId = "source/a",
			 .RelatedSourceId = {},
			 .RequestedSample = 100,
			 .AppliedSample = 100,
			 .SampleOffsetFrames = 0,
			 .Timing = "exact",
			 .FinishProvenance = {}}
		};
		return observation;
	}

	class FakeBridge final : public engine::script::DataAudioObservationBridge {
	  public:
		engine::script::DataAudioObservationBridgeCapabilities Capabilities() const override {
			return {.Available = true, .Detail = "fake bridge", .MaximumFrames = 256};
		}
		bool Capture(
			std::string_view, engine::script::DataAudioObservationBridgeResult &, std::string &
		) override {
			return false;
		}
		bool ReadWaveform(
			std::string_view,
			std::string_view,
			std::string_view,
			uint64_t,
			size_t,
			std::vector<std::byte> &,
			std::string &
		) override {
			return false;
		}
	};
}

TEST_CASE("audio observation bridge validates the exact copied record bounds", "[script][audio]") {
	std::string failure;
	CHECK(engine::script::ValidateDataAudioObservation(ValidObservation(), failure));
	CHECK(failure.empty());

	auto invalid = ValidObservation();
	invalid.Events.front().AppliedSample = 101;
	CHECK_FALSE(engine::script::ValidateDataAudioObservation(invalid, failure));
	CHECK(failure == "invalid audio event");

	invalid = ValidObservation();
	invalid.Waveform.Available = false;
	invalid.Waveform.UnavailableReason = "capture disabled";
	invalid.Waveform.ResourceId.clear();
	invalid.Waveform.Sha256.clear();
	invalid.Waveform.ByteLength = 0;
	invalid.Waveform.Chunks.clear();
	invalid.Missing = {{.Field = "waveform", .Reason = "capture disabled"}};
	CHECK(engine::script::ValidateDataAudioObservation(invalid, failure));

	invalid = ValidObservation();
	invalid.Sources.front().Kind = std::string{"\xC0\x80", 2};
	CHECK_FALSE(engine::script::ValidateDataAudioObservation(invalid, failure));
	CHECK(failure == "invalid audio source");

	invalid = ValidObservation();
	invalid.Sources.front().Kind.assign(engine::script::MAX_AUDIO_OBSERVATION_STRING_BYTES, 'x');
	CHECK(engine::script::ValidateDataAudioObservation(invalid, failure));

	invalid = ValidObservation();
	invalid.Events.clear();
	for (size_t index = 0; index < 5'250; index++) {
		invalid.Events.push_back(
			{.EventId = "event/" + std::to_string(index),
			 .Kind = "play",
			 .SourceId = "source/a",
			 .RelatedSourceId = {},
			 .RequestedSample = 100,
			 .AppliedSample = 100,
			 .SampleOffsetFrames = 0,
			 .Timing = "exact",
			 .FinishProvenance = {}}
		);
	}
	CHECK_FALSE(engine::script::ValidateDataAudioObservation(invalid, failure));
	CHECK(failure == "audio observation exceeds aggregate bounds");
}

TEST_CASE("data-scene reports bridge capability without naming an audio implementation", "[script][audio]") {
	const auto bridge = std::make_shared<FakeBridge>();
	const engine::script::DataSceneResult capabilities =
		engine::script::GetAudioObservationCapabilities(bridge);
	CHECK(std::string_view(capabilities.Status) == "ok");
	REQUIRE(capabilities.Value.Tag == engine::script::ValueTag::Map);
	bool available = false;
	for (const auto &[name, value] : capabilities.Value.Entries)
		if (name == "audio_observation") available = value.Boolean;
	CHECK(available);
}
