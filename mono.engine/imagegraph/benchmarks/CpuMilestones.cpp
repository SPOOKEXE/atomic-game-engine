#include "../tests/fixtures/CpuMilestones.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.cpu-milestones")
namespace {
	using Fixture = engine::imagegraph::testing::CpuMilestoneFixture;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	struct Phase {
		std::string Name;
		double SelfMilliseconds = 0;
		size_t Calls = 0;
	};
	struct Reading {
		double Milliseconds = 0, UnmarkedMilliseconds = 0;
		uint64_t AllocatedBytes = 0, AllocatedBlocks = 0;
		int64_t LiveDeltaBytes = 0, ProcessPeakBytes = 0, ProfilerOverheadBytes = 0;
		std::vector<Phase> Phases;
		std::vector<engine::core::Counter> Counters;
	};
	struct ProfileFixture {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		uint64_t ExpectedFingerprint = 0;
		explicit ProfileFixture(Fixture::Kind kind) : Graph(kind) {
			// Lazy construction occurs only in the harness's discarded first warmup.
			// Compilation, repeatability preflight and recording allocations are
			// outside workload scopes.
			Graph.Run();
			ExpectedFingerprint = Graph.Fingerprint();
			Graph.Run();
			if (Graph.Fingerprint() != ExpectedFingerprint) Graph.Fail("preflight repeatability");
		}
		void Measure() {
			if (Count == Readings.size())
				throw std::runtime_error("CPU milestone profiles support one to five samples");
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			Metrics::Drain();
			FrameGraph::SetEnabled(true);
			const auto before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraph.cpu_milestone.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			const auto after = HeapProfile::Totals();
			if (FrameGraph::Dropped() || after.DroppedScopes != before.DroppedScopes)
				throw std::runtime_error("CPU milestone profile dropped scope evidence");
			Reading &reading = Readings[Count++];
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.UnmarkedMilliseconds = FrameGraph::UnmarkedMilliseconds();
			reading.AllocatedBytes = after.TotalBytes - before.TotalBytes;
			reading.AllocatedBlocks = after.TotalBlocks - before.TotalBlocks;
			reading.LiveDeltaBytes = after.LiveBytes - before.LiveBytes;
			reading.ProcessPeakBytes = after.PeakBytes;
			reading.ProfilerOverheadBytes = after.OverheadBytes;
			for (const auto &span : FrameGraph::Spans()) {
				const std::string name(span.Name);
				auto phase = std::find_if(reading.Phases.begin(), reading.Phases.end(), [&](const Phase &p) {
					return p.Name == name;
				});
				if (phase == reading.Phases.end()) {
					reading.Phases.push_back({name});
					phase = std::prev(reading.Phases.end());
				}
				phase->SelfMilliseconds += span.SelfMilliseconds;
				++phase->Calls;
			}
			reading.Counters = Metrics::Drain();
			if (Graph.Fingerprint() != ExpectedFingerprint) Graph.Fail("measured repeatability");
			engine::testing::Consume(Graph.Output());
		}
		~ProfileFixture() {
			// Eight warmups are omitted. Frame time measures the workload; framework ns
			// also includes recording.
			if (Count <= 8) return;
			const size_t samples = Count - 8;
			double low = std::numeric_limits<double>::max(), high = 0, unmarked = 0;
			uint64_t allocated = 0, blocks = 0;
			int64_t liveDelta = 0, peak = 0, overhead = 0;
			std::vector<Phase> phases;
			std::vector<engine::core::Counter> counters;
			for (size_t i = 8; i < Count; ++i) {
				const auto &r = Readings[i];
				low = std::min(low, r.Milliseconds);
				high = std::max(high, r.Milliseconds);
				unmarked = std::max(unmarked, r.UnmarkedMilliseconds);
				allocated += r.AllocatedBytes;
				blocks += r.AllocatedBlocks;
				liveDelta += r.LiveDeltaBytes;
				peak = std::max(peak, r.ProcessPeakBytes);
				overhead = std::max(overhead, r.ProfilerOverheadBytes);
				for (const auto &p : r.Phases) {
					auto found = std::find_if(phases.begin(), phases.end(), [&](const Phase &other) {
						return p.Name == other.Name;
					});
					if (found == phases.end()) {
						phases.push_back(p);
						continue;
					}
					found->SelfMilliseconds += p.SelfMilliseconds;
					found->Calls += p.Calls;
				}
				for (const auto &counter : r.Counters) {
					auto found = std::find_if(counters.begin(), counters.end(), [&](const auto &other) {
						return counter.Name == other.Name;
					});
					if (found == counters.end()) {
						counters.push_back(counter);
						continue;
					}
					found->Value += counter.Value;
					found->Samples += counter.Samples;
				}
			}
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_CPU_PRESET");
			std::printf(
				"# cpu-profile preset=%s backend=cpu kind=%u nodes=%zu seed=42 project_side=%u "
				"ticks=0..%llu samples=%zu frame_min_ms=%.6f frame_max_ms=%.6f unmarked_max_ms=%.6f "
				"output_fnv=%llu retained_output_bytes=%llu retained_simulation_bytes=%llu heap_status=%s\n",
				preset ? preset : "unreported",
				unsigned(Graph.Workload),
				Graph.Authored.Nodes.size(),
				Fixture::Side,
				static_cast<unsigned long long>(
					Graph.Workload == Fixture::Kind::Static2D || Graph.Workload == Fixture::Kind::Mesh3D
						? 0
						: Fixture::Frames - 1
				),
				samples,
				low,
				high,
				unmarked,
				static_cast<unsigned long long>(ExpectedFingerprint),
				static_cast<unsigned long long>(Graph.RetainedOutputBytes()),
				static_cast<unsigned long long>(Graph.RetainedReplayBytes()),
				HeapProfile::IsCompiledIn() ? "available" : "unavailable"
			);
			if (HeapProfile::IsCompiledIn())
				std::printf(
					"# cpu-heap kind=%u allocated_bytes_per_call=%llu allocated_blocks_per_call=%llu "
					"live_delta_bytes_per_call=%lld process_peak_bytes=%lld profiler_overhead_bytes=%lld\n",
					unsigned(Graph.Workload),
					static_cast<unsigned long long>(allocated / samples),
					static_cast<unsigned long long>(blocks / samples),
					static_cast<long long>(liveDelta / int64_t(samples)),
					static_cast<long long>(peak),
					static_cast<long long>(overhead)
				);

			for (const auto &p : phases)
				std::printf(
					"# cpu-phase kind=%u name=%s calls_per_sample=%zu "
					"self_mean_ms=%.6f\n",
					unsigned(Graph.Workload),
					p.Name.c_str(),
					p.Calls / samples,
					p.SelfMilliseconds / samples
				);
			for (const auto &counter : counters)
				std::printf(
					"# cpu-counter kind=%u name=%.*s value_per_sample=%.0f "
					"observations_per_sample=%u\n",
					unsigned(Graph.Workload),
					int(counter.Name.Text().size()),
					counter.Name.Text().data(),
					counter.Value / samples,
					unsigned(counter.Samples / samples)
				);
		}
	};
} // namespace
BENCH("CPU static2D 128x128 nodes2 seed42 tick0 compiled-plan evaluation", 1) {
	static ProfileFixture f(Fixture::Kind::Static2D);
	f.Measure();
}
BENCH(
	"CPU animation 128x128 nodes2 seed42 ticks0..15 frames16 compiled-plan "
	"evaluation",
	1
) {
	static ProfileFixture f(Fixture::Kind::Animation);
	f.Measure();
}
BENCH(
	"CPU temporal feedback 128x128 nodes2 seed42 ticks0..15 frames16 "
	"reset-and-stream",
	1
) {
	static ProfileFixture f(Fixture::Kind::Feedback);
	f.Measure();
}
BENCH(
	"CPU fixed simulation grid8x8 bounds32x32 points81 edges144 nodes2 substeps4 seed42 "
	"ticks0..15 reset-and-stream",
	1
) {
	static ProfileFixture f(Fixture::Kind::Simulation);
	f.Measure();
}
BENCH(
	"CPU3D preparation Icosphere-level2 vertices960 transforms2 nodes2 "
	"seed42 tick0 noGPUrender",
	1
) {
	static ProfileFixture f(Fixture::Kind::Mesh3D);
	f.Measure();
}
