// What a block costs, against the only deadline in this engine that is hard.
//
// **Every other benchmark in this repository measures something that can be
// late.** A slow frame is a stutter; a slow tick is a rubber band. A slow audio
// block is a *click*, because the device does not wait - it plays whatever is
// in the ring buffer, and if the mixer has not filled it, what it plays is the
// last block again or silence. There is no graceful degradation available.
//
// So the unit that matters here is **percent of the block period**, and the
// arithmetic is fixed: `DEFAULT_BLOCK_FRAMES` is 512 and the default rate is
// 48 kHz, so one block is 512 / 48000 = **10.67 milliseconds**, and `Render`
// has to finish inside that or the callback is late. A row reporting 100 µs is
// using 0.94% of the budget; a row reporting 5 ms is using half of it and is
// one scheduling hiccup from an audible fault.
//
// **The voice ladder is the whole suite.** A game does not decide how many
// sounds are playing - the player does, by walking into a firefight - so the
// question is not "what does the mixer cost" but "how many voices before the
// budget is gone". The ladder answers that directly, and the number it produces
// is the one a voice cap should be set from rather than guessed at.
//
// Nothing here touches a device. `Render` fills a caller's buffer and that is
// the whole of the audio path that can be measured without hardware, which is
// also why it is worth measuring: on a machine with no sound card this is the
// only thing in the module that runs at all.

#include <engine/audio/Commands.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Mixer.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/audio/Spatial.hpp>
#include <engine/testing/Bench.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.audio.bench.mixing")

using engine::audio::AudioFormat;
using engine::audio::AudioMixer;
using engine::audio::Command;
using engine::audio::CommandKind;
using engine::audio::DEFAULT_BLOCK_FRAMES;
using engine::audio::EmitterPlacement;
using engine::audio::MixReport;
using engine::audio::NodeId;
using engine::audio::NodeKind;
using engine::audio::SampleBuffer;
using engine::audio::SoundRef;
using engine::audio::StereoGain;
using engine::testing::Consume;

namespace mixing_bench {

	// One block, at the default block size. The deadline every row is read
	// against: 512 frames at 48 kHz is 10.67 ms.
	constexpr size_t BLOCK = DEFAULT_BLOCK_FRAMES;

	// A second of audio, which is longer than any block so a player never
	// reaches the end mid-benchmark and takes the finishing path instead of the
	// mixing one.
	SoundRef Tone() {
		static const SoundRef tone = [] {
			AudioFormat format;
			std::vector<float> samples(format.SampleRate * format.Channels);
			for (size_t frame = 0; frame * format.Channels < samples.size(); frame++) {
				// A real waveform rather than a constant. A mixer summing
				// constants can be vectorised in ways one summing varying data
				// cannot, and the flattering version is not the one that ships.
				const auto phase = static_cast<float>(frame) * 0.01f;
				for (uint16_t channel = 0; channel < format.Channels; channel++) {
					samples[frame * format.Channels + channel] = std::sin(phase + channel);
				}
			}
			return std::make_shared<const SampleBuffer>(format, std::span<const float>(samples));
		}();
		return tone;
	}

	// A mixer with `voices` players, each through its own fader into the output.
	//
	// **Built once per shape and reused**, because constructing a graph
	// allocates and a device thread never does. `ApplyPending` rather than
	// posting through the queue and rendering, so the graph is in a known state
	// before the first measured block.
	AudioMixer &GraphOf(size_t voices, bool spatial) {
		static std::vector<std::pair<std::pair<size_t, bool>, std::unique_ptr<AudioMixer>>> built;
		for (auto &[key, mixer] : built) {
			if (key.first == voices && key.second == spatial) {
				return *mixer;
			}
		}

		auto mixer = std::make_unique<AudioMixer>();
		engine::audio::AudioGraph &graph = mixer->Graph();
		const NodeId output{engine::audio::AudioGraph::OUTPUT_ID};

		for (size_t voice = 0; voice < voices; voice++) {
			const NodeId player = graph.Add(NodeKind::Player);
			const NodeId stage = graph.Add(spatial ? NodeKind::Emitter : NodeKind::Fader);

			if (engine::audio::Node *node = graph.Find(player)) {
				node->Sound = Tone();
				node->Playing = true;
				node->Looping = true;
				// Distinct cursors, so the voices are not reading the same
				// cache line of the same sound in lockstep - which is what a
				// real mix looks like and what makes the memory traffic honest.
				node->Cursor = static_cast<double>(voice * 37u % 40'000u);
				node->Gain = 0.5f;
			}

			if (engine::audio::Node *node = graph.Find(stage)) {
				node->Gain = 0.5f;
				if (spatial) {
					// Scattered around the listener and inside the falloff
					// window, so distance attenuation actually computes rather
					// than short-circuiting on "silent" or "full volume".
					const auto offset = static_cast<float>(voice);
					node->Placement = EmitterPlacement{
						std::sin(offset) * 20.0f, 0.0f, std::cos(offset) * 20.0f, 5.0f, 60.0f
					};
				} else {
					node->Pan = std::sin(static_cast<float>(voice)) * 0.8f;
				}
			}

			graph.Connect(player, stage);
			graph.Connect(stage, output);
		}

		built.emplace_back(std::make_pair(voices, spatial), std::move(mixer));
		return *built.back().second;
	}

	// The output buffer, held the way a device callback holds one.
	SampleBuffer &Out() {
		static SampleBuffer out(AudioFormat{}, BLOCK);
		return out;
	}
}

using namespace mixing_bench;

// --- the voice ladder -----------------------------------------------------------
//
// **One iteration is one block.** The reported figure is nanoseconds per block,
// and the budget is 10 670 000 of them. Divide to get the share; the first row
// that passes a few percent is where a voice cap starts to matter, and the row
// that passes 100% is where the audio breaks.

BENCH("Render · 1 voice", 100) {
	AudioMixer &mixer = GraphOf(1, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 16 voices", 100) {
	AudioMixer &mixer = GraphOf(16, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 64 voices", 100) {
	AudioMixer &mixer = GraphOf(64, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 256 voices", 100) {
	// A busy scene: a firefight, a crowd, a machine room. Nothing stops a game
	// reaching this, which is exactly why the number has to exist before
	// somebody's player finds it.
	AudioMixer &mixer = GraphOf(256, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 512 voices", 50) {
	// Half of `MAXIMUM_NODES` spent on players alone. **If this row is inside
	// the budget, the graph's own bound is the effective voice cap and no
	// separate one is needed**; if it is outside, the cap has to come from
	// somewhere and this row says where.
	AudioMixer &mixer = GraphOf(512, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 50; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

// --- spatial against flat -------------------------------------------------------
//
// An `Emitter` does distance attenuation and equal-power panning against the
// listener; a `Fader` multiplies. Same voice count, so the gap is exactly what
// placing a sound in the world costs - and it is the number that decides
// whether ambience should be spatial or simply mixed.

BENCH("Render · 64 spatial voices", 100) {
	AudioMixer &mixer = GraphOf(64, true);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 256 spatial voices", 100) {
	AudioMixer &mixer = GraphOf(256, true);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

// --- commands inside the block --------------------------------------------------
//
// **A command with a deadline inside the block splits it.** That is the whole
// point of the sample-accurate scheduling - a sound starts on the sample it was
// meant to, not on the block boundary - and it is not free: each split is
// another pass over the graph for a shorter run of frames. `MixReport::Segments`
// counts the pieces, and one means nothing was scheduled inside, which the
// header calls the common case. These rows are what the uncommon case costs.

BENCH("Render · 64 voices, no commands", 100) {
	AudioMixer &mixer = GraphOf(64, false);
	SampleBuffer &out = Out();
	size_t segments = 0;
	for (size_t block = 0; block < 100; block++) {
		segments += mixer.Render(out).Segments;
	}
	Consume(segments);
}

BENCH("Render · 64 voices, 8 commands split across the block", 100) {
	AudioMixer &mixer = GraphOf(64, false);
	SampleBuffer &out = Out();
	engine::audio::CommandQueue &queue = mixer.Commands();

	size_t segments = 0;
	for (size_t block = 0; block < 100; block++) {
		const uint64_t base = mixer.Clock();
		for (size_t index = 0; index < 8; index++) {
			Command command;
			command.Kind = CommandKind::SetGain;
			command.Target = NodeId{engine::audio::AudioGraph::OUTPUT_ID};
			command.Value = 0.5f;
			// Spread across the block, so each one forces a separate split
			// rather than all landing on the same sample.
			command.AtSample = base + (BLOCK / 8) * index;
			Consume(queue.Post(command));
		}
		segments += mixer.Render(out).Segments;
	}
	Consume(segments);
}

BENCH("Render · 64 voices, 64 commands split across the block", 100) {
	// **The pathological case: a split every eight frames.** A tick that
	// scheduled a great many things inside one block gets this, and the shape to
	// look for is whether the cost grows with the *splits* or with the frames -
	// if a 64-way split costs eight times a 8-way one, the per-segment overhead
	// dominates and there is a fixed cost per segment worth amortising.
	AudioMixer &mixer = GraphOf(64, false);
	SampleBuffer &out = Out();
	engine::audio::CommandQueue &queue = mixer.Commands();

	size_t segments = 0;
	for (size_t block = 0; block < 100; block++) {
		const uint64_t base = mixer.Clock();
		for (size_t index = 0; index < 64; index++) {
			Command command;
			command.Kind = CommandKind::SetGain;
			command.Target = NodeId{engine::audio::AudioGraph::OUTPUT_ID};
			command.Value = 0.5f;
			command.AtSample = base + (BLOCK / 64) * index;
			Consume(queue.Post(command));
		}
		segments += mixer.Render(out).Segments;
	}
	Consume(segments);
}

// --- the queue ------------------------------------------------------------------

BENCH("CommandQueue::Post · 100k", 100'000) {
	// **Paid on the tick thread, not the device thread**, so it is not against
	// the audio deadline - but a world posting a command per sound per tick does
	// it a great many times, and a full queue drops rather than blocks. This is
	// what posting costs and, by extension, how long the producer holds the ring.
	static engine::audio::CommandQueue queue;
	static std::vector<Command> drained;

	Command command;
	command.Kind = CommandKind::SetGain;
	command.Target = NodeId{engine::audio::AudioGraph::OUTPUT_ID};
	command.Value = 1.0f;

	size_t posted = 0;
	for (size_t index = 0; index < 100'000; index++) {
		posted += queue.Post(command) ? 1u : 0u;
		// Drained every so often, because the ring is bounded at 1024 and a
		// benchmark measuring a permanently full queue would measure the
		// refusal path and call it posting.
		if ((index & 0x1FFu) == 0x1FFu) {
			drained.clear();
			Consume(queue.Drain(drained));
		}
	}
	Consume(posted);
}

BENCH("CommandQueue::Post · 100k against a full ring", 100'000) {
	// The refusal path. A full queue drops the command and says so rather than
	// blocking, because the producer is a tick and the consumer has a deadline.
	// Refusing has to be cheap or a world that overruns the ring pays twice.
	static engine::audio::CommandQueue queue;
	Command command;
	command.Kind = CommandKind::SetGain;
	command.Target = NodeId{engine::audio::AudioGraph::OUTPUT_ID};

	// Fill it once; nothing drains it afterwards.
	while (queue.Post(command)) {}

	size_t refused = 0;
	for (size_t index = 0; index < 100'000; index++) {
		refused += queue.Post(command) ? 0u : 1u;
	}
	Consume(refused);
}

// --- the arithmetic -------------------------------------------------------------
//
// The two functions an `Emitter` calls per voice per block. Small, and measured
// because the spatial rows above are the only other place they appear and a
// difference there could be either of them.

BENCH("PanGain · 100k", 100'000) {
	float total = 0.0f;
	for (size_t index = 0; index < 100'000; index++) {
		const StereoGain gain = engine::audio::PanGain(std::sin(static_cast<float>(index) * 0.001f));
		total += gain.Left + gain.Right;
	}
	Consume(total);
}

BENCH("DistanceGain · 100k", 100'000) {
	static const EmitterPlacement placement{0.0f, 0.0f, 0.0f, 5.0f, 60.0f};
	float total = 0.0f;
	for (size_t index = 0; index < 100'000; index++) {
		total += engine::audio::DistanceGain(static_cast<float>(index % 80), placement);
	}
	Consume(total);
}

BENCH("SampleBuffer::MixFrom · 100 blocks", 100) {
	// The primitive a `Bus` is made of: sum one buffer into another with a gain.
	// A 64-voice mix is 64 of these per block, so this row times the voice count
	// should account for most of the ladder above - and whatever it does not
	// account for is the graph walk.
	static const SampleBuffer source(AudioFormat{}, BLOCK);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		out.Silence();
		Consume(out.MixFrom(source, 0.5f));
	}
}
