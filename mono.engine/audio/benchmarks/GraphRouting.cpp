// Prepared audio routing construction and sample-deadline command dispatch.
// No device participates; output sample and ordering oracles are checked.

#include <engine/audio/Graph.hpp>
#include <engine/audio/Mixer.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.audio.bench.graphrouting")

namespace {
	using namespace engine::audio;
	using engine::testing::Consume;

	void BuildRouting(size_t voices) {
		AudioGraph graph;
		const NodeId output = graph.Output();
		std::vector<std::pair<NodeId, NodeId>> paths;
		paths.reserve(voices);
		for (size_t index = 0; index < voices; ++index) {
			const NodeId player = graph.Add(NodeKind::Player);
			const NodeId fader = graph.Add(NodeKind::Fader);
			if (!player.IsValid() || !fader.IsValid() || !graph.Connect(player, fader) ||
				!graph.Connect(fader, output))
				throw std::runtime_error("audio routing build refused");
			paths.emplace_back(player, fader);
		}
		const std::span<const NodeId> order = graph.Order();
		if (order.size() != 1 + voices * 2 || order.back() != output)
			throw std::runtime_error("audio routing order changed");
		for (const auto &[player, fader] : paths) {
			const auto first = std::find(order.begin(), order.end(), player);
			const auto second = std::find(order.begin(), order.end(), fader);
			if (first >= second || second >= order.end() - 1)
				throw std::runtime_error("audio routing dependency order changed");
		}
		Consume(order.data());
	}

	float SummedSample(const std::array<float, 3> &levels, const std::array<size_t, 3> &wires) {
		AudioMixer mixer(AudioFormat{}, 128);
		std::array<NodeId, 3> players;
		for (size_t index = 0; index < players.size(); ++index) {
			players[index] = mixer.Graph().Add(NodeKind::Player);
			if (!players[index].IsValid()) throw std::runtime_error("audio summation fixture node refused");
			std::vector<float> samples(256 * AudioFormat{}.Channels, levels[index]);
			Node *node = mixer.Graph().Find(players[index]);
			node->Sound = std::make_shared<const SampleBuffer>(AudioFormat{}, samples);
			node->Playing = true;
		}
		for (size_t index : wires)
			if (!mixer.Graph().Connect(players[index], mixer.Graph().Output()))
				throw std::runtime_error("audio summation fixture wire refused");
		SampleBuffer output(AudioFormat{}, 128);
		if (mixer.Render(output).Frames != 128)
			throw std::runtime_error("audio summation fixture render refused");
		return output.Frame(0)[0];
	}

	[[maybe_unused]] const bool SUMMATION_VERIFIED = [] {
		constexpr std::array levels{1.0e20f, -1.0e20f, 1.0f};
		if (SummedSample(levels, {0, 1, 2}) != 1.0f || SummedSample(levels, {0, 2, 1}) != 0.0f)
			throw std::runtime_error("audio authored summation order changed");
		return true;
	}();

	struct DeadlineFixture {
		AudioMixer Mixer{AudioFormat{}, 128};
		SampleBuffer Output{AudioFormat{}, 128};
		NodeId Player;
		float CurrentGain = 1.0f;
		bool NextLow = true;

		DeadlineFixture() {
			Player = Mixer.Graph().Add(NodeKind::Player);
			if (!Player.IsValid() || !Mixer.Graph().Connect(Player, Mixer.Graph().Output()))
				throw std::runtime_error("audio deadline fixture routing refused");
			std::vector<float> samples(4096 * AudioFormat{}.Channels, 0.5f);
			Node *node = Mixer.Graph().Find(Player);
			node->Sound = std::make_shared<const SampleBuffer>(AudioFormat{}, samples);
			node->Playing = true;
			node->Looping = true;
		}

		void RenderScheduled() {
			const float next = NextLow ? 0.5f : 1.0f;
			Command gain;
			gain.Kind = CommandKind::SetGain;
			gain.AtSample = Mixer.Clock() + 32;
			gain.Target = Player;
			gain.Value = next;
			if (!Mixer.Commands().Post(gain)) throw std::runtime_error("audio deadline command refused");
			const MixReport report = Mixer.Render(Output);
			if (report.Frames != 128 || report.Applied != 1 || report.Segments != 2 ||
				Output.Frame(31)[0] != 0.5f * CurrentGain || Output.Frame(32)[0] != 0.5f * next)
				throw std::runtime_error("audio sample deadline or output changed");
			CurrentGain = next;
			NextLow = !NextLow;
			Consume(Output.Data().data());
		}
	};

	DeadlineFixture &Deadline() {
		static DeadlineFixture fixture;
		return fixture;
	}

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("audio heap probe requires MONO_HEAP_PROFILE");
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
		if (!found) throw std::runtime_error("benchmark heap scope was not recorded");
		std::cout << "# b0 heap\t" << name << '\t' << (after.TotalBytes - before.TotalBytes) << '\t'
				  << (after.TotalBlocks - before.TotalBlocks) << '\t' << (after.LiveBytes - before.LiveBytes)
				  << '\t' << after.LiveBytes << '\t' << peak << '\t' << after.LiveBlocks << '\t'
				  << after.PeakBytes << '\t' << after.OverheadBytes << '\t' << HeapProfile::BlockOverhead()
				  << '\t' << (after.DroppedScopes - before.DroppedScopes) << '\t'
				  << (after.ForeignFrees - before.ForeignFrees) << '\n';
	}

	void ProbeMemoryOnce() {
		static const bool probed = [] {
			if (const char *setting = std::getenv("MONO_BENCH_MEMORY");
				setting != nullptr && setting[0] == '1' && setting[1] == '\0') {
				std::cout << "# b0 heap basis\ttracked_cpp_requested_payload=1\t"
						  << "malloc_gpu_excluded=1\twork_invocations=1\tfixtures_prepared=1\t"
						  << "scope_peak=exclusive\tprocess_peak=lifetime\t"
						  << "external_contention_invalidates_timing=1\ttiming_valid=0\t"
						  << "counters=process_local\n";
				std::cout
					<< "# b0 heap "
					   "v2\tname\tprocess_allocated_bytes\tprocess_allocated_blocks\tprocess_retained_bytes\t"
					   "process_live_bytes\tscope_self_peak_bytes\tprocess_live_blocks\tprocess_peak_"
					   "lifetime_bytes\t"
					   "process_header_overhead_model_bytes\tblock_overhead_bytes\tdropped_scopes_"
					   "delta\tforeign_"
					   "frees_delta\n";
				ProbeHeap("b0 audio output", [] { BuildRouting(0); });
				ProbeHeap("b0 audio simple", [] { BuildRouting(1); });
				ProbeHeap("b0 audio busy", [] { BuildRouting(16); });
				ProbeHeap("b0 audio complex", [] { BuildRouting(64); });
				ProbeHeap("b0 audio mixer construction", [] {
					AudioMixer mixer(AudioFormat{}, 128);
					Consume(mixer.Clock());
				});
				Deadline();
				ProbeHeap("b0 audio deadline cold", [] { Deadline().RenderScheduled(); });
				ProbeHeap("b0 audio deadline warm", [] { Deadline().RenderScheduled(); });
			}
			return true;
		}();
		(void)probed;
	}
}

BENCH("AudioGraph output only prepared", 1) {
	ProbeMemoryOnce();
	BuildRouting(0);
}
BENCH("AudioGraph one voice routing", 1) {
	BuildRouting(1);
}
BENCH("AudioGraph sixteen voice routing", 1) {
	BuildRouting(16);
}
BENCH("AudioGraph sixty-four voice routing", 1) {
	BuildRouting(64);
}
BENCH("Render scheduled gain 128 frames", 1) {
	Deadline().RenderScheduled();
}
