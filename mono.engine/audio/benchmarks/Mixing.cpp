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
#include <engine/core/HeapProfile.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
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

	bool LatencyMode() {
		const char *mode = std::getenv("MONO_B0_LATENCY");
		if (!mode || std::string_view(mode) == "0") return false;
		if (std::string_view(mode) != "1") throw std::runtime_error("MONO_B0_LATENCY must be 0 or 1");
		const char *memory = std::getenv("MONO_BENCH_MEMORY");
		if (memory && std::string_view(memory) != "0")
			throw std::runtime_error("B0 latency requires MONO_BENCH_MEMORY=0");
		if (engine::core::HeapProfile::IsCompiledIn())
			throw std::runtime_error("B0 latency requires heap profiling compiled out");
		return true;
	}

	size_t LatencyOption(const char *name, size_t fallback, size_t minimum, size_t maximum) {
		const char *value = std::getenv(name);
		if (!value) return fallback;
		const std::string_view text(value);
		size_t result = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result < minimum ||
			result > maximum)
			throw std::runtime_error(std::string(name) + " is outside its bounded integer range");
		return result;
	}

	template <class Value> void PrintTails(const char *name, std::vector<Value> samples) {
		if (samples.empty()) throw std::runtime_error("B0 latency cohort has no observations");
		std::sort(samples.begin(), samples.end());
		const auto rank = [&](size_t percent) { return samples[(samples.size() * percent + 99) / 100 - 1]; };
		std::cout << ' ' << name << "_samples=" << samples.size() << ' ' << name << "_p50=" << rank(50) << ' '
				  << name << "_p95=" << rank(95) << ' ' << name << "_p99=" << rank(99) << ' ' << name
				  << "_max=" << samples.back();
	}

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

	struct VoiceInput {
		float Pan = 0.0f;
		EmitterPlacement Placement{};
	};

	// grug author once so the mixer and scalar oracle receive identical stored float inputs.
	std::vector<VoiceInput> BuildInputs(size_t voices, bool spatial) {
		std::vector<VoiceInput> inputs(voices);
		for (size_t voice = 0; voice < voices; ++voice) {
			const float offset = static_cast<float>(voice);
			if (spatial) {
				// grug keep emitters inside the falloff window, where distance and panning both do work.
				inputs[voice].Placement =
					EmitterPlacement{std::sin(offset) * 20.0f, 0.0f, std::cos(offset) * 20.0f, 5.0f, 60.0f};
			} else {
				inputs[voice].Pan = std::sin(offset) * 0.8f;
			}
		}
		return inputs;
	}

	// grug build routing outside measured Render calls, including first-block probes.
	std::unique_ptr<AudioMixer> BuildMixer(std::span<const VoiceInput> inputs, bool spatial) {
		const size_t voices = inputs.size();
		auto mixer = std::make_unique<AudioMixer>();
		engine::audio::AudioGraph &graph = mixer->Graph();
		const NodeId output{engine::audio::AudioGraph::OUTPUT_ID};
		size_t activeVoices = 0;

		for (size_t voice = 0; voice < voices; voice++) {
			const NodeId player = graph.Add(NodeKind::Player);
			const NodeId stage = graph.Add(spatial ? NodeKind::Emitter : NodeKind::Fader);
			if (!player.IsValid() || !stage.IsValid()) {
				throw std::runtime_error("audio benchmark graph refused a voice node");
			}

			if (engine::audio::Node *node = graph.Find(player)) {
				node->Sound = Tone();
				node->Playing = true;
				node->Looping = true;
				// Distinct cursors, so the voices are not reading the same
				// cache line of the same sound in lockstep - which is what a
				// real mix looks like and what makes the memory traffic honest.
				node->Cursor = static_cast<double>(voice * 37u % 40'000u);
				node->Gain = 0.5f;
				++activeVoices;
			} else {
				throw std::runtime_error("audio benchmark graph lost a player node");
			}

			if (engine::audio::Node *node = graph.Find(stage)) {
				node->Gain = 0.5f;
				if (spatial)
					node->Placement = inputs[voice].Placement;
				else
					node->Pan = inputs[voice].Pan;
			} else {
				throw std::runtime_error("audio benchmark graph lost a voice stage");
			}

			if (!graph.Connect(player, stage) || !graph.Connect(stage, output)) {
				throw std::runtime_error("audio benchmark graph refused a voice connection");
			}
		}
		const size_t expectedNodes = 1 + voices * 2;
		if (activeVoices != voices || graph.Count() != expectedNodes ||
			graph.Order().size() != expectedNodes) {
			throw std::runtime_error("audio benchmark graph did not activate the requested voices");
		}

		return mixer;
	}

	// grug keep timing fixtures separate from the diagnostic first/next block mixers.
	AudioMixer &GraphOf(size_t voices, bool spatial) {
		static std::vector<std::pair<std::pair<size_t, bool>, std::unique_ptr<AudioMixer>>> built;
		for (auto &[key, mixer] : built) {
			if (key.first == voices && key.second == spatial) return *mixer;
		}
		const auto inputs = BuildInputs(voices, spatial);
		built.emplace_back(std::make_pair(voices, spatial), BuildMixer(inputs, spatial));
		return *built.back().second;
	}

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("runtime heap probe requires MONO_HEAP_PROFILE");
		// grug single owner and no device threads: reset makes nested tags share one call's process peak.
		HeapProfile::ResetTotals();
		const auto before = HeapProfile::Totals();
		{
			ENGINE_HEAP_SCOPE(name);
			work();
		}
		const auto after = HeapProfile::Totals();
		int64_t peak = 0;
		bool found = false;
		for (uint32_t index = 1; index < HeapProfile::NodeCount(); ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Parent == HeapProfile::ROOT && node.Name == name) {
				found = true;
				peak = node.PeakBytes;
			}
		}
		if (!found) throw std::runtime_error("runtime benchmark heap scope was not recorded");
		std::cout << "# b0 heap\t" << name << '\t' << (after.TotalBytes - before.TotalBytes) << '\t'
				  << (after.TotalBlocks - before.TotalBlocks) << '\t' << (after.LiveBytes - before.LiveBytes)
				  << '\t' << after.LiveBytes << '\t' << peak << '\t' << after.LiveBlocks << '\t'
				  << after.PeakBytes << '\t' << before.LiveBytes << '\t'
				  << (after.PeakBytes - before.LiveBytes) << '\t' << after.OverheadBytes << '\t'
				  << HeapProfile::BlockOverhead() << '\t' << (after.DroppedScopes - before.DroppedScopes)
				  << '\t' << (after.ForeignFrees - before.ForeignFrees) << '\n';
	}

	void PrintHeapBasis() {
		std::cout << "# b0 heap basis\ttracked_cpp_requested_payload=1\t"
				  << "malloc_gpu_excluded=1\twork_invocations=1\tfixtures_prepared=1\t"
				  << "reference_setup_excluded=1\toutput_retained_at_snapshot=1\t"
				  << "scope_peak=exclusive\tprocess_peak=single_call_since_reset\t"
				  << "fixtures_references_resident=1\tpeak_above_entry=additional_tracked_payload_not_rss\t"
				  << "external_contention_invalidates_timing=1\ttiming_valid=0\t"
				  << "counters=process_local\n";
		std::cout << "# b0 heap v3\tname\tprocess_allocated_bytes\tprocess_allocated_blocks\t"
				  << "process_retained_bytes\tprocess_live_bytes\tscope_self_peak_bytes\t"
				  << "process_live_blocks\tprocess_peak_since_reset_bytes\tbaseline_live_bytes\t"
				  << "process_peak_above_entry_live_bytes\t"
				  << "process_header_overhead_model_bytes\tblock_overhead_bytes\t"
				  << "dropped_scopes_delta\tforeign_frees_delta\n";
	}

	struct MixReference {
		std::array<float, BLOCK * 2> Samples{};
		float Peak = 0.0f;
		bool Clipped = false;
	};

	// grug scalar oracle sums authored voice order, then clips once at the output.
	MixReference ReferenceMix(
		std::span<const VoiceInput> inputs, bool spatial, uint64_t beginSample, float outputGain = 1.0f
	) {
		MixReference expected;
		const SoundRef source = Tone();
		for (size_t voice = 0; voice < inputs.size(); ++voice) {
			const StereoGain gain =
				spatial ? engine::audio::Place(engine::audio::ListenerPose{}, inputs[voice].Placement)
						: engine::audio::PanGain(inputs[voice].Pan);
			for (size_t frame = 0; frame < BLOCK; ++frame) {
				const size_t cursor = (voice * 37u % 40'000u + beginSample + frame) % source->Frames();
				const auto sample = source->Frame(cursor);
				const float left = sample[0] * 0.5f;
				const float right = sample[1] * 0.5f;
				// grug force the stored stage's float rounding before the output sum; no fused multiply-add.
				const volatile float stageLeft = left * (0.5f * gain.Left);
				const volatile float stageRight = right * (0.5f * gain.Right);
				expected.Samples[frame * 2] += stageLeft;
				expected.Samples[frame * 2 + 1] += stageRight;
			}
		}
		for (float &sample : expected.Samples) {
			if (outputGain != 1.0f) sample *= outputGain;
			expected.Peak = std::max(expected.Peak, std::abs(sample));
			expected.Clipped |= sample > 1.0f || sample < -1.0f;
			sample = std::clamp(sample, -1.0f, 1.0f);
		}
		return expected;
	}

	uint64_t SampleHash(const SampleBuffer &output) {
		uint64_t hash = 14695981039346656037ULL;
		for (float sample : output.Data()) {
			const uint32_t bits = std::bit_cast<uint32_t>(sample);
			for (uint32_t shift = 0; shift < 32; shift += 8) {
				hash ^= (bits >> shift) & 0xffu;
				hash *= 1099511628211ULL;
			}
		}
		return hash;
	}

	void MeasureLatency(size_t voices, bool spatial, size_t commands, const char *workload) {
		const size_t trials = LatencyOption("MONO_B0_TRIALS", 101, 1, 512);
		const size_t warmups = LatencyOption("MONO_B0_WARMUPS", 8, 0, 128);
		const auto inputs = BuildInputs(voices, spatial);
		auto mixer = BuildMixer(inputs, spatial);
		SampleBuffer output(AudioFormat{}, BLOCK);
		std::vector<uint64_t> durations;
		durations.reserve(trials);
		for (size_t trial = 0; trial < warmups + trials; ++trial) {
			const uint64_t begin = mixer->Clock();
			const uint64_t serial = mixer->ObservationSerial();
			for (size_t index = 0; index < commands; ++index) {
				Command command;
				command.Kind = CommandKind::SetGain;
				command.Target = mixer->Graph().Output();
				command.Value = .5f;
				command.AtSample = begin + (BLOCK / commands) * index;
				if (!mixer->Commands().Post(command))
					throw std::runtime_error("B0 audio command queue refused");
			}
			const auto started = std::chrono::steady_clock::now();
			const auto report = mixer->Render(output);
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
									 std::chrono::steady_clock::now() - started
			)
									 .count();
			// Oracle generation and every sample/trace comparison follow the clock.
			const auto expected = ReferenceMix(inputs, spatial, begin, commands ? .5f : 1.0f);
			if (!std::equal(
					output.Data().begin(),
					output.Data().end(),
					expected.Samples.begin(),
					expected.Samples.end()
				) ||
				report.Frames != BLOCK || report.Applied != commands ||
				report.Segments != std::max(size_t{1}, commands) || report.Peak != expected.Peak ||
				report.Clipped != expected.Clipped || report.Finished != 0 || report.BeginSample != begin ||
				report.EndSample != begin + BLOCK || mixer->Clock() != begin + BLOCK ||
				report.ObservationSerial != serial + 1 ||
				mixer->ObservationSerial() != report.ObservationSerial || mixer->Commands().Dropped() != 0 ||
				!mixer->LastFinishedSources().empty() || mixer->LastAppliedCommands().size() != commands)
				throw std::runtime_error("B0 audio full sample/report oracle failed");
			for (size_t index = 0; index < commands; ++index) {
				const auto &event = mixer->LastAppliedCommands()[index];
				const auto offset = (BLOCK / commands) * index;
				if (event.Kind != CommandKind::SetGain || event.Target != mixer->Graph().Output() ||
					event.RequestedSample != begin + offset || event.AppliedSample != begin + offset ||
					event.OffsetFrames != offset || event.Status != engine::audio::PlaybackStatus::Applied)
					throw std::runtime_error("B0 audio sample-deadline trace oracle failed");
			}
			if (elapsed <= 0) throw std::runtime_error("B0 audio clock did not advance");
			if (trial >= warmups) durations.push_back(static_cast<uint64_t>(elapsed));
		}
		std::cout << "b0-audio-latency workload=" << workload << " voices=" << voices
				  << " spatial=" << spatial << " commands=" << commands << " frames=" << BLOCK
				  << " warmups=" << warmups
				  << " measurement=individual_mixer_render_not_device_callback_scheduling device=none "
					 "full_samples=1 full_trace=1"
				  << " oracle_outside_clock=1 harness_summary=not_individual_latency quantile=nearest_rank "
					 "heap=compiled_out quiet_host=unverified"
				  << " tail_valid=" << (trials >= 100)
				  << " callback_period_ns=" << double(BLOCK) * 1e9 / AudioFormat{}.SampleRate;
		PrintTails("render_ns", durations);
		std::cout << '\n';
	}

	void ProbeMix(const char *firstName, const char *nextName, size_t voices, bool spatial) {
		const auto inputs = BuildInputs(voices, spatial);
		auto mixer = BuildMixer(inputs, spatial);
		SampleBuffer output(AudioFormat{}, BLOCK);
		uint64_t previousSerial = mixer->ObservationSerial();
		for (const char *name : {firstName, nextName}) {
			const uint64_t beginSample = mixer->Clock();
			const MixReference expected = ReferenceMix(inputs, spatial, beginSample);
			MixReport report;
			ProbeHeap(name, [&] { report = mixer->Render(output); });
			if (output.Data().size() != expected.Samples.size() ||
				!std::equal(output.Data().begin(), output.Data().end(), expected.Samples.begin()) ||
				report.Frames != BLOCK || report.Applied != 0 || report.Segments != 1 ||
				report.Finished != 0 || report.Peak != expected.Peak || report.Clipped != expected.Clipped ||
				report.BeginSample != beginSample || report.EndSample != beginSample + BLOCK ||
				mixer->Clock() != beginSample + BLOCK || report.ObservationSerial != previousSerial + 1 ||
				mixer->ObservationSerial() != report.ObservationSerial ||
				!mixer->LastAppliedCommands().empty() || !mixer->LastFinishedSources().empty()) {
				const auto actual = output.Data();
				const size_t sharedSamples = std::min(actual.size(), expected.Samples.size());
				size_t firstMismatch = 0;
				while (firstMismatch < sharedSamples &&
					   actual[firstMismatch] == expected.Samples[firstMismatch])
					++firstMismatch;
				std::cerr << "# b0 audio mismatch\t" << name
						  << "\texpected_samples=" << expected.Samples.size()
						  << "\tactual_samples=" << actual.size();
				if (firstMismatch < sharedSamples)
					std::cerr << "\tsample_index=" << firstMismatch << "\texpected_bits="
							  << std::bit_cast<uint32_t>(expected.Samples[firstMismatch])
							  << "\tactual_bits=" << std::bit_cast<uint32_t>(actual[firstMismatch]);
				else
					std::cerr << "\tsample_index=none";
				std::cerr << "\texpected_peak_bits=" << std::bit_cast<uint32_t>(expected.Peak)
						  << "\tactual_peak_bits=" << std::bit_cast<uint32_t>(report.Peak)
						  << "\texpected_clipped=" << expected.Clipped
						  << "\tactual_clipped=" << report.Clipped << "\tframes=" << report.Frames
						  << "\tapplied=" << report.Applied << "\tsegments=" << report.Segments
						  << "\tfinished=" << report.Finished << "\texpected_begin=" << beginSample
						  << "\tactual_begin=" << report.BeginSample
						  << "\texpected_end=" << beginSample + BLOCK << "\tactual_end=" << report.EndSample
						  << "\tclock=" << mixer->Clock() << "\texpected_serial=" << previousSerial + 1
						  << "\treport_serial=" << report.ObservationSerial
						  << "\tmixer_serial=" << mixer->ObservationSerial()
						  << "\tapplied_commands=" << mixer->LastAppliedCommands().size()
						  << "\tfinished_sources=" << mixer->LastFinishedSources().size() << '\n'
						  << std::flush;
				throw std::runtime_error("audio memory probe samples, report or clock changed");
			}
			previousSerial = report.ObservationSerial;
			std::cout << "# b0 runtime oracle\t" << name << "\tfull_samples=1\tvoices=" << voices
					  << "\tspatial=" << spatial << "\tframes=" << report.Frames
					  << "\tbegin_sample=" << report.BeginSample << "\tend_sample=" << report.EndSample
					  << "\tpeak=" << report.Peak << "\tclipped=" << report.Clipped
					  << "\tsample_fnv1a64=" << SampleHash(output) << '\n';
		}
	}

	void ProbeMemoryOnce() {
		static const bool probed = [] {
			const char *setting = std::getenv("MONO_BENCH_MEMORY");
			if (setting == nullptr || setting[0] != '1' || setting[1] != '\0') return true;
			PrintHeapBasis();
			ProbeMix(
				"b0 audio runtime empty prepared first", "b0 audio runtime empty prepared next", 0, false
			);
			ProbeMix(
				"b0 audio runtime simple prepared first", "b0 audio runtime simple prepared next", 1, false
			);
			ProbeMix(
				"b0 audio runtime busy prepared first", "b0 audio runtime busy prepared next", 64, false
			);
			ProbeMix(
				"b0 audio runtime complex prepared first",
				"b0 audio runtime complex prepared next",
				511,
				false
			);
			ProbeMix(
				"b0 audio runtime spatial prepared first", "b0 audio runtime spatial prepared next", 64, true
			);
			return true;
		}();
		(void)probed;
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
	if (LatencyMode()) {
		MeasureLatency(0, false, 0, "empty");
		MeasureLatency(1, false, 0, "Render-1_voice");
		return;
	}
	ProbeMemoryOnce();
	AudioMixer &mixer = GraphOf(1, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 16 voices", 100) {
	if (LatencyMode()) {
		MeasureLatency(16, false, 0, "Render-16_voices");
		return;
	}
	AudioMixer &mixer = GraphOf(16, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 64 voices", 100) {
	if (LatencyMode()) {
		MeasureLatency(64, false, 0, "Render-64_voices");
		return;
	}
	AudioMixer &mixer = GraphOf(64, false);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 256 voices", 100) {
	if (LatencyMode()) {
		MeasureLatency(256, false, 0, "Render-256_voices");
		return;
	}
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

BENCH("Render · 511 voices", 50) {
	if (LatencyMode()) {
		MeasureLatency(511, false, 0, "Render-511_voices");
		return;
	}
	// 511 players, 511 stages, and the output use 1023 of 1024 node slots.
	// **If this row is inside the budget, the graph's own bound is the effective
	// voice cap and no separate one is needed**; if it is outside, the cap has to
	// come from somewhere and this row says where.
	AudioMixer &mixer = GraphOf(511, false);
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
	if (LatencyMode()) {
		MeasureLatency(64, true, 0, "Render-64_spatial_voices");
		return;
	}
	AudioMixer &mixer = GraphOf(64, true);
	SampleBuffer &out = Out();
	for (size_t block = 0; block < 100; block++) {
		const MixReport report = mixer.Render(out);
		Consume(report.Peak);
	}
}

BENCH("Render · 256 spatial voices", 100) {
	if (LatencyMode()) {
		MeasureLatency(256, true, 0, "Render-256_spatial_voices");
		return;
	}
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
	if (LatencyMode()) {
		MeasureLatency(64, false, 0, "Render-64_voices,_no_commands");
		return;
	}
	AudioMixer &mixer = GraphOf(64, false);
	SampleBuffer &out = Out();
	size_t segments = 0;
	for (size_t block = 0; block < 100; block++) {
		segments += mixer.Render(out).Segments;
	}
	Consume(segments);
}

BENCH("Render · 64 voices, 8 commands split across the block", 100) {
	if (LatencyMode()) {
		MeasureLatency(64, false, 8, "Render-64_voices,_8_commands_split_across_the_block");
		return;
	}
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
	if (LatencyMode()) {
		MeasureLatency(64, false, 64, "Render-64_voices,_64_commands_split_across_the_block");
		return;
	}
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
