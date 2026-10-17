#include <engine/audio/Commands.hpp>
#include <engine/audio/Device.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

// The pipeline end to end, with no sound card.
//
// **This is what the null device is for.** Every stage above the hardware is
// data - bytes in, samples out - so the only part that genuinely needs a device
// is the handover, and that part is small. A suite that needed real audio would
// run nowhere: CI has no sound card and a developer's is in use.

TEST_SUITE_ID("engine.audio.device")
TEST_DEPENDS("engine.audio.mixer")
TEST_DEPENDS("engine.audio.wav")

using engine::audio::AudioFormat;
using engine::audio::Command;
using engine::audio::CommandKind;
using engine::audio::DecodeWav;
using engine::audio::DeviceSettings;
using engine::audio::EmitterPlacement;
using engine::audio::ListenerPose;
using engine::audio::NodeId;
using engine::audio::NodeKind;
using engine::audio::NullDevice;
using engine::audio::OpenNullDevice;
using engine::audio::SampleBuffer;
using engine::audio::SoundRef;

namespace {
	constexpr AudioFormat STEREO{.SampleRate = 48000, .Channels = 2};
	constexpr size_t BLOCK = 32;

	DeviceSettings Settings() {
		DeviceSettings settings;
		settings.Format = STEREO;
		settings.BlockFrames = BLOCK;
		return settings;
	}

	Command Act(CommandKind kind, NodeId target, uint64_t at = 0) {
		Command command;
		command.Kind = kind;
		command.Target = target;
		command.AtSample = at;
		return command;
	}

	Command AddNode(NodeId id, NodeKind kind) {
		Command command = Act(CommandKind::AddNode, id);
		command.Node = kind;
		return command;
	}

	Command Wire(NodeId from, NodeId to) {
		Command command = Act(CommandKind::Connect, from);
		command.Second = to;
		return command;
	}

	// A real `.wav` file, built in memory - the same bytes a delivery client
	// would hand over after fetching one.
	std::vector<std::byte> WavFile(const std::vector<int16_t> &samples, uint16_t channels = 2) {
		std::vector<std::byte> file;
		const auto put32 = [&file](uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				file.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
			}
		};
		const auto put16 = [&file](uint16_t value) {
			file.push_back(static_cast<std::byte>(value & 0xFF));
			file.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
		};
		const auto tag = [&file](const char *text) {
			for (int index = 0; index < 4; ++index) {
				file.push_back(static_cast<std::byte>(text[index]));
			}
		};

		const auto dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
		tag("RIFF");
		put32(36 + dataBytes);
		tag("WAVE");
		tag("fmt ");
		put32(16);
		put16(1);
		put16(channels);
		put32(48000);
		put32(48000u * channels * 2u);
		put16(static_cast<uint16_t>(channels * 2));
		put16(16);
		tag("data");
		put32(dataBytes);
		for (const int16_t sample : samples) {
			put16(static_cast<uint16_t>(sample));
		}
		return file;
	}
}

TEST_CASE("a null device renders on demand and never on its own", "[audio][device]") {
	// No thread and no hardware: a suite states how much time passed rather
	// than waiting for it, which is the discipline `net` applies to timeouts.
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	REQUIRE(device != nullptr);
	CHECK(device->Running());
	CHECK(device->Rendered() == 0);

	CHECK(device->Advance(1) == BLOCK);
	CHECK(device->Rendered() == BLOCK);

	CHECK(device->Advance(4) == BLOCK * 4);
	CHECK(device->Rendered() == BLOCK * 5);
}

TEST_CASE("a paused null device preserves its sample clock", "[audio][device]") {
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	device->Advance(1);
	const uint64_t before = device->Rendered();
	device->SetPaused(true);
	CHECK(device->Paused());
	CHECK(device->Advance(4) == 0);
	CHECK(device->Rendered() == before);
	device->SetPaused(false);
	CHECK_FALSE(device->Paused());
	CHECK(device->Advance(1) == BLOCK);
	CHECK(device->Rendered() == before + BLOCK);
}

TEST_CASE("a null device with nothing playing is silent", "[audio][device]") {
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	device->Advance(3);
	CHECK(device->LastBlock().Peak() == 0.0f);
}

TEST_CASE("the whole pipeline runs from wav bytes to samples", "[audio][device][e2e]") {
	// **The path a delivered sound actually takes**: bytes an origin served,
	// decoded, converted to the mixer's format, wired into a graph, scheduled,
	// and rendered. Every stage in one case, because each of them is unit
	// tested apart and none of that proves they agree.
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &mixer = device->Mixer();

	// Half scale, so the result is checkable by eye.
	const std::vector<std::byte> file = WavFile(std::vector<int16_t>(256, 16384));
	const auto decoded = DecodeWav(file);
	REQUIRE(decoded.has_value());

	const auto sound = std::make_shared<const SampleBuffer>(decoded->ConvertTo(STEREO));
	REQUIRE(sound->Frames() == 128);

	const NodeId player = mixer.Commands().Allocate();
	REQUIRE(mixer.Commands().Post(AddNode(player, NodeKind::Player)));
	REQUIRE(mixer.Commands().Post(Wire(player, mixer.Graph().Output())));

	Command sourced = Act(CommandKind::SetSound, player);
	sourced.Sound = sound;
	REQUIRE(mixer.Commands().Post(sourced));
	REQUIRE(mixer.Commands().Post(Act(CommandKind::Play, player)));

	device->Advance(1);
	CHECK(device->LastBlock().Frame(0)[0] == 0.5f);
	CHECK(device->LastBlock().Frame(BLOCK - 1)[0] == 0.5f);
}

TEST_CASE("a spatialised sound moves between the ears", "[audio][device][e2e]") {
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &mixer = device->Mixer();

	std::vector<float> samples(4096 * 2, 1.0f);
	const auto sound = std::make_shared<const SampleBuffer>(STEREO, samples);

	const NodeId player = mixer.Commands().Allocate();
	const NodeId emitter = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(player, NodeKind::Player));
	mixer.Commands().Post(AddNode(emitter, NodeKind::Emitter));
	mixer.Commands().Post(Wire(player, emitter));
	mixer.Commands().Post(Wire(emitter, mixer.Graph().Output()));

	Command sourced = Act(CommandKind::SetSound, player);
	sourced.Sound = sound;
	mixer.Commands().Post(sourced);
	mixer.Commands().Post(Act(CommandKind::Play, player));

	// To the listener's right.
	Command right = Act(CommandKind::SetPlacement, emitter);
	right.Placement = EmitterPlacement{.X = 3.0f, .Y = 0.0f, .Z = 0.0f};
	mixer.Commands().Post(right);

	device->Advance(1);
	CHECK(device->LastBlock().Frame(0)[1] > device->LastBlock().Frame(0)[0]);

	// And to the left.
	Command left = Act(CommandKind::SetPlacement, emitter);
	left.Placement = EmitterPlacement{.X = -3.0f, .Y = 0.0f, .Z = 0.0f};
	mixer.Commands().Post(left);

	device->Advance(1);
	CHECK(device->LastBlock().Frame(0)[0] > device->LastBlock().Frame(0)[1]);
}

TEST_CASE("moving the listener changes what is heard", "[audio][device][e2e]") {
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &mixer = device->Mixer();

	std::vector<float> samples(4096 * 2, 1.0f);
	const auto sound = std::make_shared<const SampleBuffer>(STEREO, samples);

	const NodeId player = mixer.Commands().Allocate();
	const NodeId emitter = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(player, NodeKind::Player));
	mixer.Commands().Post(AddNode(emitter, NodeKind::Emitter));
	mixer.Commands().Post(Wire(player, emitter));
	mixer.Commands().Post(Wire(emitter, mixer.Graph().Output()));

	Command sourced = Act(CommandKind::SetSound, player);
	sourced.Sound = sound;
	mixer.Commands().Post(sourced);
	mixer.Commands().Post(Act(CommandKind::Play, player));

	Command near = Act(CommandKind::SetPlacement, emitter);
	near.Placement = EmitterPlacement{.X = 0.0f, .Y = 0.0f, .Z = 0.0f};
	mixer.Commands().Post(near);

	device->Advance(1);
	const float close = device->LastBlock().Peak();
	CHECK(close > 0.0f);

	// Walk the listener a long way off. The emitter has not moved.
	Command moved;
	moved.Kind = CommandKind::SetListener;
	moved.Pose = ListenerPose{.X = 1000.0f};
	mixer.Commands().Post(moved);

	device->Advance(1);
	CHECK(device->LastBlock().Peak() == 0.0f);
}

// --- the queue's own contract ---------------------------------------------

TEST_CASE("a full queue drops rather than blocking", "[audio][device]") {
	// The producer is a tick and the consumer has a deadline: waiting would
	// stall the world to keep a sound, which is the wrong way round.
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &queue = device->Mixer().Commands();

	const NodeId node = queue.Allocate();
	size_t posted = 0;
	while (queue.Post(Act(CommandKind::Play, node))) {
		++posted;
		REQUIRE(posted < 100000);
	}

	CHECK(posted > 0);
	CHECK(queue.Dropped() > 0);

	// And it recovers once the mixer has drained it.
	device->Advance(1);
	CHECK(queue.Post(Act(CommandKind::Play, node)));
}

TEST_CASE("ids are unique across allocations", "[audio][device]") {
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &queue = device->Mixer().Commands();

	std::vector<NodeId> issued;
	for (int index = 0; index < 500; ++index) {
		const NodeId id = queue.Allocate();
		REQUIRE(id.IsValid());
		for (const NodeId seen : issued) {
			REQUIRE_FALSE(seen == id);
		}
		issued.push_back(id);
	}
	// And none of them is the output's reserved id.
	for (const NodeId id : issued) {
		REQUIRE_FALSE(id == device->Mixer().Graph().Output());
	}
}

TEST_CASE("a command posted from another thread arrives intact", "[audio][device]") {
	// **The arrangement the whole module is shaped around**: one producer, one
	// consumer, no lock. This is the case that would catch a torn command - a
	// slot published before it was filled.
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &mixer = device->Mixer();

	std::vector<float> samples(4096 * 2, 0.5f);
	const auto sound = std::make_shared<const SampleBuffer>(STEREO, samples);

	const NodeId player = mixer.Commands().Allocate();

	// The producer runs on its own thread while the consumer drains here.
	std::thread producer([&]() {
		mixer.Commands().Post(AddNode(player, NodeKind::Player));
		mixer.Commands().Post(Wire(player, mixer.Graph().Output()));

		Command sourced = Act(CommandKind::SetSound, player);
		sourced.Sound = sound;
		mixer.Commands().Post(sourced);
		mixer.Commands().Post(Act(CommandKind::Play, player));
	});
	producer.join();

	device->Advance(1);
	CHECK(device->LastBlock().Frame(0)[0] == 0.5f);
	// The sound survived the hand-off: the node holds a reference of its own.
	REQUIRE(mixer.Graph().Find(player) != nullptr);
	CHECK(mixer.Graph().Find(player)->Sound != nullptr);
}

TEST_CASE("the queue does not keep sounds alive after handing them over", "[audio][device]") {
	// A queue holding the last reference to every sound ever played is a leak
	// that looks like a cache.
	std::unique_ptr<NullDevice> device = OpenNullDevice(Settings());
	auto &mixer = device->Mixer();

	std::weak_ptr<const SampleBuffer> watch;
	const NodeId player = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(player, NodeKind::Player));

	{
		std::vector<float> samples(64 * 2, 0.25f);
		const auto sound = std::make_shared<const SampleBuffer>(STEREO, samples);
		watch = sound;

		Command sourced = Act(CommandKind::SetSound, player);
		sourced.Sound = sound;
		mixer.Commands().Post(sourced);
	}

	device->Advance(1);
	// The node holds it now.
	CHECK_FALSE(watch.expired());

	// Drop it from the node too, and drain.
	Command cleared = Act(CommandKind::SetSound, player);
	mixer.Commands().Post(cleared);
	device->Advance(1);
	CHECK(watch.expired());
}

TEST_CASE("a real device is optional and its absence is not an error", "[audio][device]") {
	// A CI container has no sound server. A game that refused to start because
	// it could not make a noise would be worse than one that is quiet - so
	// this asserts the contract rather than that a device exists.
	const std::unique_ptr<engine::audio::Device> device = engine::audio::OpenDevice(Settings());
	if (device == nullptr) {
		SUCCEED("no audio output on this machine, which is a supported outcome");
		return;
	}
	CHECK(device->Running());
	CHECK(device->Format().Channels == STEREO.Channels);
	device->Close();
	CHECK_FALSE(device->Running());
	// Twice is safe.
	device->Close();
}

TEST_CASE(
	"null device exposes copied seek and playback acknowledgements", "[audio][device][playback_events]"
) {
	using namespace engine::audio;
	DeviceSettings settings = Settings();
	settings.PlaybackEvents = true;
	auto device = OpenNullDevice(settings);
	auto &queue = device->Mixer().Commands();
	const NodeId player = queue.Allocate();
	REQUIRE(queue.Free() >= 5);
	REQUIRE(queue.Post(AddNode(player, NodeKind::Player)));
	REQUIRE(queue.Post(Wire(player, device->Mixer().Graph().Output())));
	const std::array<float, 4> samples{0.125f, 0.25f, 0.5f, 0.75f};
	SoundRef sound =
		std::make_shared<const SampleBuffer>(AudioFormat{.SampleRate = 24000, .Channels = 1}, samples);
	Command command = Act(CommandKind::SetSound, player);
	command.Sound = sound;
	command.PlaybackGeneration = 11;
	REQUIRE(queue.Post(command));
	command = Act(CommandKind::Seek, player, 2);
	command.CursorFrames = 1.5;
	command.PlaybackGeneration = 11;
	REQUIRE(queue.Post(command));
	command.Kind = CommandKind::Play;
	REQUIRE(queue.Post(command));
	CHECK(device->Advance() == BLOCK);
	CHECK(device->LastBlock().Frame(1)[0] == 0.0f);
	CHECK(device->LastBlock().Frame(2)[0] == 0.25f);
	CHECK(device->LastBlock().Frame(3)[0] == 0.5f);
	std::array<PlaybackEvent, 8> events{};
	REQUIRE(device->Mixer().PollPlaybackEvents(events) == 6);
	CHECK(events[3].CursorFrames == 1.5);
	CHECK(events[3].Applied.AppliedSample == 2);
	CHECK(events[5].NaturalCompletion);
	CHECK(events[5].Finished.AtSample == 7);
	CHECK(events[5].PlaybackGeneration == 11);
	const long referencesBeforePoll = sound.use_count();
	CHECK(device->Mixer().PollPlaybackEvents(events) == 0);
	CHECK(sound.use_count() == referencesBeforePoll);
	command = Act(CommandKind::RemoveNode, player);
	command.PlaybackGeneration = 11;
	REQUIRE(queue.Post(command));
	device->Advance();
	REQUIRE(device->Mixer().PollPlaybackEvents(events) == 1);
	CHECK_FALSE(events[0].Present);
	CHECK(events[0].PlaybackGeneration == 11);
	// Host ownership survives all callback commands and the state acknowledgement.
	CHECK(sound.use_count() == 1);
	device->Close();
	CHECK(sound->Frame(0)[0] == 0.125f);
}

TEST_CASE("SDL dummy playback copies honor the device opt in", "[audio][device][sdl_dummy]") {
	using namespace engine::audio;
	const char *driver = std::getenv("SDL_AUDIODRIVER");
	if (driver == nullptr || std::string_view(driver) != "dummy")
		SKIP("run with SDL_AUDIODRIVER=dummy to test the headless SDL callback");
	for (const bool enabled : {true, false}) {
		CAPTURE(enabled);
		DeviceSettings settings = Settings();
		settings.PlaybackEvents = enabled;
		auto device = OpenDevice(settings);
		REQUIRE(device != nullptr);
		REQUIRE(device->SetPaused(true));
		auto &queue = device->Mixer().Commands();
		const NodeId player = queue.Allocate();
		REQUIRE(queue.Free() >= 5);
		REQUIRE(queue.Post(AddNode(player, NodeKind::Player)));
		REQUIRE(queue.Post(Wire(player, NodeId{AudioGraph::OUTPUT_ID})));
		SoundRef sound =
			std::make_shared<const SampleBuffer>(device->Format(), std::vector<float>(8192, 0.25f));
		Command command = Act(CommandKind::SetSound, player);
		command.Sound = sound;
		command.PlaybackGeneration = 7;
		REQUIRE(queue.Post(command));
		command = Act(CommandKind::Seek, player);
		command.CursorFrames = 1.5;
		command.PlaybackGeneration = 7;
		REQUIRE(queue.Post(command));
		command.Kind = CommandKind::Play;
		REQUIRE(queue.Post(command));
		const uint64_t before = device->Rendered();
		REQUIRE(device->SetPaused(false));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (device->Rendered() == before && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		REQUIRE(device->SetPaused(true));
		REQUIRE(device->Rendered() > before);
		device->Mixer().ApplyPending();
		std::array<PlaybackEvent, 16> events{};
		const size_t count = device->Mixer().PollPlaybackEvents(events);
		if (enabled) {
			REQUIRE(count >= 5);
			CHECK(events[0].Applied.Kind == CommandKind::AddNode);
			CHECK(events[1].Applied.Kind == CommandKind::Connect);
			CHECK(events[2].Applied.Kind == CommandKind::SetSound);
			CHECK(events[3].Applied.Kind == CommandKind::Seek);
			CHECK(events[3].CursorFrames == 1.5);
			CHECK(events[3].PlaybackGeneration == 7);
			CHECK(events[4].Applied.Kind == CommandKind::Play);
			CHECK(events[4].Playing);
			for (size_t index = 0; index < 5; ++index) {
				CHECK(events[index].Applied.Target == player);
				CHECK(events[index].Status == PlaybackStatus::Applied);
			}
		} else {
			CHECK(count == 0);
		}
		CHECK(device->Mixer().PlaybackEventsDropped() == 0);
		device->Close();
		device.reset();
	}
}
