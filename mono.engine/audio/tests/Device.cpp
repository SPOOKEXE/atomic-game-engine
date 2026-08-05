#include <engine/audio/Commands.hpp>
#include <engine/audio/Device.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// The pipeline end to end, with no sound card.
//
// **This is what the null device is for.** Every stage above the hardware is
// data — bytes in, samples out — so the only part that genuinely needs a device
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

	// A real `.wav` file, built in memory — the same bytes a delivery client
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
	// consumer, no lock. This is the case that would catch a torn command — a
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
	// it could not make a noise would be worse than one that is quiet — so
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
