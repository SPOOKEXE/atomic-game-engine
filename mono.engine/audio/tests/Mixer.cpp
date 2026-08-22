#include <engine/audio/Commands.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Mixer.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.audio.mixer")
TEST_DEPENDS("engine.audio.graph")
TEST_DEPENDS("engine.audio.sample")
TEST_DEPENDS("engine.audio.spatial")

using engine::audio::AudioFormat;
using engine::audio::AudioMixer;
using engine::audio::Command;
using engine::audio::CommandKind;
using engine::audio::CommandQueue;
using engine::audio::MixReport;
using engine::audio::NodeId;
using engine::audio::NodeKind;
using engine::audio::SampleBuffer;
using engine::audio::SoundRef;

namespace {
	constexpr AudioFormat STEREO{.SampleRate = 48000, .Channels = 2};
	constexpr size_t BLOCK = 64;

	// A sound whose every sample is the same, so what comes out of the mixer is
	// checkable by eye rather than by ear.
	SoundRef Constant(float value, size_t frames = 4096, AudioFormat format = STEREO) {
		std::vector<float> samples(frames * format.Channels, value);
		return std::make_shared<const SampleBuffer>(format, samples);
	}

	// How much one source frame is worth in a ramp.
	//
	// **Small enough that a long ramp stays inside full scale.** The obvious
	// version stores the frame index itself, and every sample past the first
	// then hits the output clipper - so the test reads 1.0 for ever and looks
	// like a cursor that stopped advancing.
	constexpr float RAMP_STEP = 1.0f / 4096.0f;

	// A sound that counts, so a cursor's position is visible in the output.
	SoundRef Ramp(size_t frames, AudioFormat format = STEREO) {
		std::vector<float> samples(frames * format.Channels);
		for (size_t frame = 0; frame < frames; ++frame) {
			for (uint16_t channel = 0; channel < format.Channels; ++channel) {
				samples[frame * format.Channels + channel] = static_cast<float>(frame) * RAMP_STEP;
			}
		}
		return std::make_shared<const SampleBuffer>(format, samples);
	}

	// Which source frame a ramp sample came from.
	size_t FrameOf(float sample) {
		return static_cast<size_t>(std::lround(sample / RAMP_STEP));
	}

	// Commands, built rather than brace-initialised.
	//
	// C++ requires designated initialisers in declaration order, and a `Command`
	// declares `AtSample` before `Target` - so `{.Target = ..., .AtSample = ...}`
	// does not compile and the order that does read backwards. These name what
	// they do and leave the rest defaulted.
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

	Command SetSound(NodeId id, SoundRef sound) {
		Command command = Act(CommandKind::SetSound, id);
		command.Sound = std::move(sound);
		return command;
	}

	Command SetValue(CommandKind kind, NodeId id, float value, uint64_t at = 0) {
		Command command = Act(kind, id, at);
		command.Value = value;
		return command;
	}

	Command SetFlag(CommandKind kind, NodeId id, bool flag) {
		Command command = Act(kind, id);
		command.Flag = flag;
		return command;
	}

	// A mixer with one player wired straight to the output, which is the
	// smallest graph that makes a sound.
	struct Rig {
		AudioMixer Engine{STEREO, BLOCK};
		NodeId Player;
		SampleBuffer Out{STEREO, BLOCK};

		Rig() {
			Player = Engine.Commands().Allocate();
			Post(AddNode(Player, NodeKind::Player));
			Post(Wire(Player, Engine.Graph().Output()));
			Engine.ApplyPending();
		}

		void Post(const Command &command) {
			REQUIRE(Engine.Commands().Post(command));
		}

		MixReport Render() {
			return Engine.Render(Out);
		}

		float At(size_t frame) const {
			return Out.Frame(frame)[0];
		}
	};
}

TEST_CASE("a producer can ask how much room is left before it posts a burst", "[audio][mixer]") {
	// **The question `CommandQueue::Post` alone cannot answer.** A voice is five
	// commands or seven and half of one is a player wired to nothing, so a
	// producer that posts and checks per command is left holding a partial graph
	// with no way to undo it - the undo would itself be commands, into the queue
	// that just refused. `mono.client/src/Sounds.cpp` is the caller.
	AudioMixer mixer(STEREO, BLOCK);
	CommandQueue &queue = mixer.Commands();

	// One slot short of `CAPACITY`, because the ring keeps one empty so that
	// full and empty are distinguishable.
	CHECK(queue.Free() == CommandQueue::CAPACITY - 1);

	Command command;
	command.Kind = CommandKind::SetGain;
	REQUIRE(queue.Post(command));
	CHECK(queue.Free() == CommandQueue::CAPACITY - 2);
	CHECK(queue.Pending() == 1);

	while (queue.Free() > 0) {
		REQUIRE(queue.Post(command));
	}

	// The two answers agree at the boundary: nothing free is exactly the point
	// where posting starts refusing and counting drops.
	const uint64_t dropped = queue.Dropped();
	CHECK_FALSE(queue.Post(command));
	CHECK(queue.Free() == 0);
	CHECK(queue.Dropped() == dropped + 1);

	// A drain is the only thing that gives room back, and it gives all of it.
	std::vector<Command> taken;
	queue.Drain(taken);
	CHECK(taken.size() == CommandQueue::CAPACITY - 1);
	CHECK(queue.Free() == CommandQueue::CAPACITY - 1);
}

TEST_CASE("an empty graph renders silence", "[audio][mixer]") {
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	const MixReport report = mixer.Render(out);
	CHECK(report.Frames == BLOCK);
	CHECK(out.Peak() == 0.0f);
	CHECK_FALSE(report.Clipped);
}

TEST_CASE("the clock advances by what was rendered", "[audio][mixer]") {
	// The sample clock is what every command deadline is measured against, so
	// it has to count frames and not blocks.
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	CHECK(mixer.Clock() == 0);
	mixer.Render(out);
	CHECK(mixer.Clock() == BLOCK);
	mixer.Render(out);
	CHECK(mixer.Clock() == 2 * BLOCK);
}

TEST_CASE("a stopped player is silent and a playing one is not", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(0.5f)));
	rig.Engine.ApplyPending();

	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);

	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();
	rig.Render();
	CHECK(rig.At(0) == 0.5f);
}

TEST_CASE("a mismatched output format produces silence rather than a resample", "[audio][mixer]") {
	// A resample on the device thread is the wrong answer to a caller's
	// configuration mistake, and silence is at least diagnosable.
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	SampleBuffer wrong(AudioFormat{.SampleRate = 44100, .Channels = 2}, BLOCK);
	const MixReport report = rig.Engine.Render(wrong);
	CHECK(report.Frames == 0);
	CHECK(wrong.Peak() == 0.0f);
}

// --- the reason this module exists ----------------------------------------

TEST_CASE("a command lands on its sample, not on the block boundary", "[audio][mixer][schedule]") {
	// **This is the requirement DATATYPES_LIBRARIES.md §11.2 marks `!`.** A
	// game ticks at frame rate and audio runs at sample rate; a Play applied at
	// the top of whichever block comes next lands up to a block early or late,
	// and a run of footsteps is then audibly uneven.
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Engine.ApplyPending();

	// Due a quarter of the way into the first block.
	constexpr size_t AT = BLOCK / 4;
	rig.Post(Act(CommandKind::Play, rig.Player, AT));

	const MixReport report = rig.Render();
	CHECK(report.Applied == 1);
	// The block was cut in two: before the command and after it.
	CHECK(report.Segments == 2);

	// Silent up to the sample it was scheduled on, and sounding from exactly
	// there. Off-by-one either way is the jitter this exists to remove.
	for (size_t frame = 0; frame < AT; ++frame) {
		INFO("frame " << frame);
		REQUIRE(rig.At(frame) == 0.0f);
	}
	for (size_t frame = AT; frame < BLOCK; ++frame) {
		INFO("frame " << frame);
		REQUIRE(rig.At(frame) == 1.0f);
	}
}

TEST_CASE("two commands in one block are applied at their own samples", "[audio][mixer][schedule]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Engine.ApplyPending();

	rig.Post(Act(CommandKind::Play, rig.Player, 10));
	rig.Post(Act(CommandKind::Stop, rig.Player, 20));

	const MixReport report = rig.Render();
	CHECK(report.Applied == 2);
	CHECK(report.Segments == 3);

	CHECK(rig.At(9) == 0.0f);
	CHECK(rig.At(10) == 1.0f);
	CHECK(rig.At(19) == 1.0f);
	CHECK(rig.At(20) == 0.0f);
}

TEST_CASE("commands due on one sample keep the order they were posted", "[audio][mixer][schedule]") {
	// "Set the gain then play" and "play then set the gain" are different, and
	// the caller's order is the one that means something.
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Engine.ApplyPending();

	rig.Post(SetValue(CommandKind::SetGain, rig.Player, 0.25f, 8));
	rig.Post(Act(CommandKind::Play, rig.Player, 8));

	rig.Render();
	CHECK(rig.At(7) == 0.0f);
	CHECK(rig.At(8) == 0.25f);
}

TEST_CASE("a deadline in the past is applied rather than dropped", "[audio][mixer][schedule]") {
	// A tick that ran late still meant its command to happen. Dropping it turns
	// a frame hitch into a missing sound, which is worse than one a few samples
	// late.
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Engine.ApplyPending();
	rig.Render(); // clock is now BLOCK

	rig.Post(Act(CommandKind::Play, rig.Player, 0));
	rig.Render();
	CHECK(rig.At(0) == 1.0f);
}

TEST_CASE("a deadline past this block takes effect at the start of the next", "[audio][mixer][schedule]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Engine.ApplyPending();

	rig.Post(Act(CommandKind::Play, rig.Player, BLOCK * 4));

	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);

	rig.Render();
	CHECK(rig.At(0) == 1.0f);
}

TEST_CASE("a block with nothing scheduled is one segment", "[audio][mixer][schedule]") {
	// The common case, and the reason the split is not paid for unless it is
	// needed.
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	const MixReport report = rig.Render();
	CHECK(report.Segments == 1);
	CHECK(report.Applied == 0);
}

// --- playback -------------------------------------------------------------

TEST_CASE("a cursor advances one source frame per output frame at matched rates", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Ramp(1024)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	rig.Render();
	// The ramp counts frames, so the output reads back the cursor directly.
	CHECK(FrameOf(rig.At(0)) == 0);
	CHECK(FrameOf(rig.At(1)) == 1);
	CHECK(FrameOf(rig.At(BLOCK - 1)) == BLOCK - 1);
}

TEST_CASE("playback continues across blocks", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Ramp(1024)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	rig.Render();
	rig.Render();
	// The second block picks up where the first ended rather than restarting.
	CHECK(FrameOf(rig.At(0)) == BLOCK);
}

TEST_CASE("a sound that ends stops the player", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f, BLOCK / 2)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	const MixReport report = rig.Render();
	CHECK(report.Finished == 1);
	CHECK(rig.At(BLOCK / 2 - 1) == 1.0f);
	// Silent past the end rather than repeating its last frame.
	CHECK(rig.At(BLOCK / 2) == 0.0f);

	// And it stays stopped.
	const MixReport second = rig.Render();
	CHECK(second.Finished == 0);
	CHECK(rig.Out.Peak() == 0.0f);
}

TEST_CASE("a looping sound wraps instead of stopping", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Ramp(8)));
	rig.Post(SetFlag(CommandKind::SetLooping, rig.Player, true));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	const MixReport report = rig.Render();
	CHECK(report.Finished == 0);
	CHECK(FrameOf(rig.At(0)) == 0);
	CHECK(FrameOf(rig.At(7)) == 7);
	// Round again.
	CHECK(FrameOf(rig.At(8)) == 0);
	CHECK(FrameOf(rig.At(15)) == 7);
}

TEST_CASE("a player with no sound is silent rather than a crash", "[audio][mixer]") {
	Rig rig;
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();

	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);
}

TEST_CASE("setting a sound rewinds the player", "[audio][mixer]") {
	// A cursor left where it was would start the new sound part way through,
	// which is never what was meant.
	Rig rig;
	rig.Post(SetSound(rig.Player, Ramp(1024)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();
	rig.Render();

	rig.Post(SetSound(rig.Player, Ramp(1024)));
	rig.Engine.ApplyPending();
	rig.Render();
	CHECK(FrameOf(rig.At(0)) == 0);
}

TEST_CASE("a muted player still advances", "[audio][mixer]") {
	// Unmuting resumes where the sound would have been rather than where it was
	// minutes ago, which is what somebody expects of a mute button.
	Rig rig;
	rig.Post(SetSound(rig.Player, Ramp(1024)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Post(SetFlag(CommandKind::SetMuted, rig.Player, true));
	rig.Engine.ApplyPending();

	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);

	rig.Post(SetFlag(CommandKind::SetMuted, rig.Player, false));
	rig.Engine.ApplyPending();
	rig.Render();
	CHECK(FrameOf(rig.At(0)) == BLOCK);
}

// --- routing ---------------------------------------------------------------

TEST_CASE("a bus sums what is wired into it", "[audio][mixer]") {
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	const NodeId bus = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(bus, NodeKind::Bus));
	mixer.Commands().Post(Wire(bus, mixer.Graph().Output()));

	for (int index = 0; index < 3; ++index) {
		const NodeId player = mixer.Commands().Allocate();
		mixer.Commands().Post(AddNode(player, NodeKind::Player));
		mixer.Commands().Post(Wire(player, bus));
		mixer.Commands().Post(SetSound(player, Constant(0.25f)));
		mixer.Commands().Post(Act(CommandKind::Play, player));
	}
	mixer.ApplyPending();

	mixer.Render(out);
	CHECK(out.Frame(0)[0] == 0.75f);
}

TEST_CASE("a fader scales its whole subtree", "[audio][mixer]") {
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	const NodeId fader = mixer.Commands().Allocate();
	const NodeId player = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(fader, NodeKind::Fader));
	mixer.Commands().Post(AddNode(player, NodeKind::Player));
	mixer.Commands().Post(Wire(player, fader));
	mixer.Commands().Post(Wire(fader, mixer.Graph().Output()));
	mixer.Commands().Post(SetSound(player, Constant(1.0f)));
	mixer.Commands().Post(Act(CommandKind::Play, player));
	mixer.Commands().Post(SetValue(CommandKind::SetGain, fader, 0.5f));
	mixer.ApplyPending();

	mixer.Render(out);
	// A centred fader is equal-power, so a hard 0.5 gain lands at
	// 0.5 * cos(45 degrees).
	CHECK(std::abs(out.Frame(0)[0] - 0.5f * 0.70710678f) < 0.0001f);
}

TEST_CASE("a negative gain is clamped to silence rather than inverting", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Post(SetValue(CommandKind::SetGain, rig.Player, -2.0f));
	rig.Engine.ApplyPending();

	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);
}

TEST_CASE("a muted bus silences its subtree without stopping it", "[audio][mixer]") {
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	const NodeId bus = mixer.Commands().Allocate();
	const NodeId player = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(bus, NodeKind::Bus));
	mixer.Commands().Post(AddNode(player, NodeKind::Player));
	mixer.Commands().Post(Wire(player, bus));
	mixer.Commands().Post(Wire(bus, mixer.Graph().Output()));
	mixer.Commands().Post(SetSound(player, Ramp(1024)));
	mixer.Commands().Post(Act(CommandKind::Play, player));
	mixer.Commands().Post(SetFlag(CommandKind::SetMuted, bus, true));
	mixer.ApplyPending();

	mixer.Render(out);
	CHECK(out.Peak() == 0.0f);

	mixer.Commands().Post(SetFlag(CommandKind::SetMuted, bus, false));
	mixer.ApplyPending();
	mixer.Render(out);
	// The player kept going while the bus was muted.
	CHECK(FrameOf(out.Frame(0)[0]) == BLOCK);
}

TEST_CASE("a detached player makes no sound and still advances", "[audio][mixer]") {
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	const NodeId player = mixer.Commands().Allocate();
	mixer.Commands().Post(AddNode(player, NodeKind::Player));
	mixer.Commands().Post(SetSound(player, Ramp(1024)));
	mixer.Commands().Post(Act(CommandKind::Play, player));
	mixer.ApplyPending();

	mixer.Render(out);
	CHECK(out.Peak() == 0.0f);
	REQUIRE(mixer.Graph().Find(player) != nullptr);
	CHECK(mixer.Graph().Find(player)->Cursor > 0.0);
}

// --- the output stage ------------------------------------------------------

TEST_CASE("the mix is clipped exactly once, at the end", "[audio][mixer]") {
	// The graph runs in float precisely so it can exceed full scale without
	// harm; a device takes samples in range, so this is where the range starts
	// to matter.
	AudioMixer mixer(STEREO, BLOCK);
	SampleBuffer out(STEREO, BLOCK);

	for (int index = 0; index < 4; ++index) {
		const NodeId player = mixer.Commands().Allocate();
		mixer.Commands().Post(AddNode(player, NodeKind::Player));
		mixer.Commands().Post(Wire(player, mixer.Graph().Output()));
		mixer.Commands().Post(SetSound(player, Constant(0.5f)));
		mixer.Commands().Post(Act(CommandKind::Play, player));
	}
	mixer.ApplyPending();

	const MixReport report = mixer.Render(out);
	CHECK(report.Clipped);
	// The meter reads what the graph produced - 2.0 - rather than what
	// survived. Measured after clipping it would read exactly 1.0 for ever,
	// which is the number that hides the problem.
	CHECK(report.Peak == 2.0f);
	CHECK(out.Peak() == 1.0f);
}

TEST_CASE("the mixer is deterministic", "[audio][mixer]") {
	// Two runs of one arrangement produce identical samples. The order the
	// graph runs in is deterministic, so this is what proves nothing in the
	// mix depends on anything else.
	const auto run = []() {
		AudioMixer mixer(STEREO, BLOCK);
		SampleBuffer out(STEREO, BLOCK);

		const NodeId bus = mixer.Commands().Allocate();
		mixer.Commands().Post(AddNode(bus, NodeKind::Bus));
		mixer.Commands().Post(Wire(bus, mixer.Graph().Output()));
		for (int index = 0; index < 5; ++index) {
			const NodeId player = mixer.Commands().Allocate();
			mixer.Commands().Post(AddNode(player, NodeKind::Player));
			mixer.Commands().Post(Wire(player, bus));
			mixer.Commands().Post(SetSound(player, Ramp(97 + index)));
			mixer.Commands().Post(SetFlag(CommandKind::SetLooping, player, true));
			mixer.Commands().Post(Act(CommandKind::Play, player, static_cast<uint64_t>(index * 7)));
		}

		std::vector<float> collected;
		for (int block = 0; block < 8; ++block) {
			mixer.Render(out);
			collected.insert(collected.end(), out.Data().begin(), out.Data().end());
		}
		return collected;
	};

	const std::vector<float> first = run();
	const std::vector<float> second = run();
	REQUIRE(first.size() == second.size());
	for (size_t index = 0; index < first.size(); ++index) {
		REQUIRE(first[index] == second[index]);
	}
}

TEST_CASE("a node removed mid-run stops being mixed", "[audio][mixer]") {
	Rig rig;
	rig.Post(SetSound(rig.Player, Constant(1.0f)));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Engine.ApplyPending();
	rig.Render();
	CHECK(rig.Out.Peak() == 1.0f);

	rig.Post(Act(CommandKind::RemoveNode, rig.Player));
	rig.Engine.ApplyPending();
	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);
}

TEST_CASE("a command naming a node that is gone is ignored", "[audio][mixer]") {
	// A tick and the mixer are a block apart, so a command for a node that has
	// just been removed is ordinary rather than a bug.
	Rig rig;
	rig.Post(Act(CommandKind::RemoveNode, rig.Player));
	rig.Post(Act(CommandKind::Play, rig.Player));
	rig.Post(SetValue(CommandKind::SetGain, rig.Player, 0.5f));
	rig.Engine.ApplyPending();

	rig.Render();
	CHECK(rig.Out.Peak() == 0.0f);
}

TEST_CASE("a sound outlives being dropped from under a playing voice", "[audio][mixer]") {
	// The reason a `SoundRef` is a shared pointer to something immutable: a
	// library may drop a sound while a voice is still walking through it, and
	// the voice must not have its samples freed underneath.
	Rig rig;
	{
		SoundRef sound = Constant(0.5f);
		rig.Post(SetSound(rig.Player, sound));
		rig.Post(Act(CommandKind::Play, rig.Player));
		rig.Engine.ApplyPending();
	}
	// The only other reference is gone. The node still holds one.
	rig.Render();
	CHECK(rig.At(0) == 0.5f);
}
