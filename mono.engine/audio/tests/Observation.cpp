#include <engine/audio/Commands.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Mixer.hpp>
#include <engine/audio/Observation.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.audio.observation")
TEST_DEPENDS("engine.audio.mixer")
TEST_DEPENDS("engine.audio.spatial")

using engine::audio::AudioFormat;
using engine::audio::AudioMixer;
using engine::audio::AudioObservation;
using engine::audio::AudioObservationLimits;
using engine::audio::AudioObservationResult;
using engine::audio::AudioObservationStatus;
using engine::audio::AudioSourceBinding;
using engine::audio::CaptureAudioObservation;
using engine::audio::Command;
using engine::audio::CommandKind;
using engine::audio::EmitterPlacement;
using engine::audio::MixReport;
using engine::audio::NodeId;
using engine::audio::NodeKind;
using engine::audio::SampleBuffer;
using engine::audio::SoundRef;

namespace {
	constexpr AudioFormat STEREO{.SampleRate = 48000, .Channels = 2};
	constexpr size_t BLOCK = 8;

	SoundRef Constant(float value, size_t frames = BLOCK * 2) {
		return std::make_shared<const SampleBuffer>(
			STEREO, std::vector<float>(frames * STEREO.Channels, value)
		);
	}

	Command Act(CommandKind kind, NodeId target, uint64_t at = 0) {
		Command command;
		command.Kind = kind;
		command.AtSample = at;
		command.Target = target;
		return command;
	}

	Command Add(NodeId target, NodeKind kind) {
		Command command = Act(CommandKind::AddNode, target);
		command.Node = kind;
		return command;
	}

	Command Connect(NodeId from, NodeId to) {
		Command command = Act(CommandKind::Connect, from);
		command.Second = to;
		return command;
	}

	struct SpatialRig {
		AudioMixer Mixer{STEREO, BLOCK};
		SampleBuffer Output{STEREO, BLOCK};
		NodeId Player = Mixer.Commands().Allocate();
		NodeId Emitter = Mixer.Commands().Allocate();

		SpatialRig() {
			Post(Add(Player, NodeKind::Player));
			Post(Add(Emitter, NodeKind::Emitter));
			Post(Connect(Player, Emitter));
			Post(Connect(Emitter, Mixer.Graph().Output()));

			Command sound = Act(CommandKind::SetSound, Player);
			sound.Sound = Constant(0.5f);
			Post(sound);

			Command placement = Act(CommandKind::SetPlacement, Emitter);
			placement.Placement = EmitterPlacement{
				.X = 3.0f,
				.Y = 4.0f,
				.Z = 0.0f,
				.FalloffStart = 1.0f,
				.FalloffEnd = 20.0f,
			};
			Post(placement);
			REQUIRE(Mixer.ApplyPending() == 6);
		}

		void Post(const Command &command) {
			REQUIRE(Mixer.Commands().Post(command));
		}

		std::vector<AudioSourceBinding> Bindings() const {
			return {
				{.SourceId = "scene/source/voice", .Node = Player},
				{.SourceId = "scene/source/emitter", .Node = Emitter},
			};
		}
	};
}

static_assert(!std::is_assignable_v<AudioObservation &, AudioObservation>);

TEST_CASE(
	"an audio observation aligns waveform sources and events to one sample clock", "[audio][observation]"
) {
	SpatialRig rig;
	rig.Post(Act(CommandKind::Play, rig.Player, 3));

	const MixReport report = rig.Mixer.Render(rig.Output);
	const std::vector<AudioSourceBinding> bindings = rig.Bindings();
	const AudioObservationResult captured = CaptureAudioObservation(rig.Mixer, rig.Output, report, bindings);

	REQUIRE(captured.Status == AudioObservationStatus::Ready);
	REQUIRE(captured.Value.has_value());
	const AudioObservation &observation = *captured.Value;
	CHECK(observation.BeginSample == 0);
	CHECK(observation.EndSample == BLOCK);
	CHECK(observation.SampleRate == 48000);
	CHECK(observation.Channels == 2);
	CHECK(observation.ChannelLayout == "stereo");
	CHECK(observation.SampleType == "float32");
	CHECK(observation.Interleaved);
	CHECK(observation.Waveform.size() == BLOCK * 2);
	CHECK(observation.Waveform[0] == 0.0f);
	CHECK(observation.Waveform[3 * 2] > 0.0f);

	REQUIRE(observation.Events.size() == 1);
	CHECK(observation.Events[0].Kind == "play");
	CHECK(observation.Events[0].SourceId == "scene/source/voice");
	CHECK(observation.Events[0].SourceIdentified);
	CHECK_FALSE(observation.Events[0].RelatedSourceIdentified);
	CHECK(observation.Events[0].RequestedSample == 3);
	CHECK(observation.Events[0].AppliedSample == 3);
	CHECK(observation.Events[0].OffsetFrames == 3);
	CHECK(observation.Events[0].Timing == "exact");

	REQUIRE(observation.Sources.size() == 2);
	CHECK(observation.Sources[0].Present);
	CHECK(observation.Sources[0].Kind == "player");
	CHECK(observation.Sources[0].Playing);
	CHECK(observation.Sources[1].Kind == "emitter");
	CHECK(observation.Sources[1].HasEmitterPlacement);
	CHECK(observation.Sources[1].ListenerDistance == 5.0f);
	CHECK(observation.AttenuationProvenance.Available);
	CHECK_FALSE(observation.OcclusionProvenance.Available);
	CHECK_FALSE(observation.OcclusionProvenance.Reason.empty());
}

TEST_CASE("an audio observation reports commands that landed early or late", "[audio][observation]") {
	SpatialRig rig;
	rig.Post(Act(CommandKind::Play, rig.Player, 100));
	MixReport report = rig.Mixer.Render(rig.Output);
	auto bindings = rig.Bindings();
	const AudioObservationResult early = CaptureAudioObservation(rig.Mixer, rig.Output, report, bindings);
	REQUIRE(early.Value.has_value());
	REQUIRE(early.Value->Events.size() == 1);
	CHECK(early.Value->Events[0].AppliedSample == BLOCK);
	CHECK(early.Value->Events[0].Timing == "early");

	rig.Post(Act(CommandKind::Stop, rig.Player, 2));
	report = rig.Mixer.Render(rig.Output);
	const AudioObservationResult late = CaptureAudioObservation(rig.Mixer, rig.Output, report, bindings);
	REQUIRE(late.Value.has_value());
	REQUIRE(late.Value->Events.size() == 1);
	CHECK(late.Value->Events[0].AppliedSample == BLOCK);
	CHECK(late.Value->Events[0].Timing == "late");
}

TEST_CASE(
	"an audio observation identifies the exact sample where a source finishes", "[audio][observation]"
) {
	SpatialRig rig;
	Command shortSound = Act(CommandKind::SetSound, rig.Player);
	shortSound.Sound = Constant(0.5f, 2);
	rig.Post(shortSound);
	rig.Post(Act(CommandKind::Play, rig.Player));
	REQUIRE(rig.Mixer.ApplyPending() == 2);

	const MixReport report = rig.Mixer.Render(rig.Output);
	const auto bindings = rig.Bindings();
	const AudioObservationResult captured = CaptureAudioObservation(rig.Mixer, rig.Output, report, bindings);

	CHECK(report.Finished == 1);
	REQUIRE(captured.Value.has_value());
	REQUIRE(captured.Value->Events.size() == 1);
	CHECK(captured.Value->Events[0].Kind == "playback_finished");
	CHECK(captured.Value->Events[0].SourceId == "scene/source/voice");
	CHECK(captured.Value->Events[0].AppliedSample == 2);
	CHECK(captured.Value->Events[0].OffsetFrames == 2);
}

TEST_CASE("a source finishing on the block boundary is reported in that block", "[audio][observation]") {
	SpatialRig rig;
	Command exactSound = Act(CommandKind::SetSound, rig.Player);
	exactSound.Sound = Constant(0.5f, BLOCK);
	rig.Post(exactSound);
	rig.Post(Act(CommandKind::Play, rig.Player));
	REQUIRE(rig.Mixer.ApplyPending() == 2);

	const MixReport report = rig.Mixer.Render(rig.Output);
	const auto bindings = rig.Bindings();
	const AudioObservationResult captured = CaptureAudioObservation(rig.Mixer, rig.Output, report, bindings);

	CHECK(report.Finished == 1);
	REQUIRE(captured.Value.has_value());
	REQUIRE(captured.Value->Events.size() == 1);
	CHECK(captured.Value->Events[0].Kind == "playback_finished");
	CHECK(captured.Value->Events[0].AppliedSample == BLOCK);
	CHECK(captured.Value->Events[0].OffsetFrames == BLOCK);
}

TEST_CASE(
	"audio observation validation refuses stale ambiguous and oversized captures", "[audio][observation]"
) {
	SpatialRig rig;
	const MixReport first = rig.Mixer.Render(rig.Output);
	const auto bindings = rig.Bindings();

	CHECK(
		CaptureAudioObservation(
			rig.Mixer,
			rig.Output,
			first,
			bindings,
			AudioObservationLimits{.MaximumFrames = BLOCK - 1, .MaximumSources = 2}
		)
			.Status == AudioObservationStatus::WaveformLimitExceeded
	);

	const std::vector<AudioSourceBinding> duplicate{
		{.SourceId = "same", .Node = rig.Player},
		{.SourceId = "same", .Node = rig.Emitter},
	};
	CHECK(
		CaptureAudioObservation(rig.Mixer, rig.Output, first, duplicate).Status ==
		AudioObservationStatus::DuplicateSourceBinding
	);
	const std::vector<AudioSourceBinding> embeddedNull{
		{.SourceId = std::string_view("source\0id", 9), .Node = rig.Player},
	};
	CHECK(
		CaptureAudioObservation(rig.Mixer, rig.Output, first, embeddedNull).Status ==
		AudioObservationStatus::InvalidSourceBinding
	);

	rig.Post(Act(CommandKind::SetGain, rig.Player));
	REQUIRE(rig.Mixer.ApplyPending() == 1);
	CHECK(
		CaptureAudioObservation(rig.Mixer, rig.Output, first, bindings).Status ==
		AudioObservationStatus::StaleMixReport
	);

	rig.Mixer.Render(rig.Output);
	CHECK(
		CaptureAudioObservation(rig.Mixer, rig.Output, first, bindings).Status ==
		AudioObservationStatus::StaleMixReport
	);
}
