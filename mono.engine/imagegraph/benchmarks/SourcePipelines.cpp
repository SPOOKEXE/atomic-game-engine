#include "../tests/fixtures/SourcePipelineWorkloads.hpp"

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

TEST_SUITE_ID("engine.imagegraph.bench.source-pipelines")
namespace {
	using Fixture = engine::imagegraph::testing::SourcePipelineFixture;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	struct Phase {
		std::string Name;
		double SelfMilliseconds = 0;
		double Calls = 0;
	};
	struct RecordedCounter {
		engine::core::Name Name;
		double Value = 0, Samples = 0;
	};
	struct Reading {
		double Milliseconds = 0, UnmarkedMilliseconds = 0;
		double AllocatedBytes = 0, AllocatedBlocks = 0;
		double LiveDeltaBytes = 0, ProcessPeakBytes = 0, ProfilerOverheadBytes = 0;
		std::vector<Phase> Phases;
		std::vector<RecordedCounter> Counters;
	};
	struct ProfileFixture {
		Fixture Graph;
		Reading Summary;
		size_t Calls = 0, Samples = 0;
		double Low = std::numeric_limits<double>::max(), High = 0;
		static constexpr size_t MaximumNames = 128;
		uint64_t ExpectedFingerprint = 0;
		explicit ProfileFixture(Fixture::Kind kind) : Graph(kind) {
			// Lazy construction occurs only in the harness's discarded first warmup.
			// Compilation, repeatability preflight and recording allocations are
			// outside workload scopes.
			Summary.Phases.reserve(MaximumNames);
			Summary.Counters.reserve(MaximumNames);
			Graph.Run();
			Graph.Validate();
			ExpectedFingerprint = Fixture::Hash(Graph.Expected);
			Graph.Run();
			if (Graph.Fingerprint() != ExpectedFingerprint) Graph.Fail("preflight repeatability");
		}
		void Measure() {
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
				ENGINE_PROFILE("imagegraph.source_pipeline.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			const auto after = HeapProfile::Totals();
			if (FrameGraph::Dropped() || after.DroppedScopes != before.DroppedScopes)
				throw std::runtime_error("Source pipeline profile dropped scope evidence");

			// EvaluateStateful enters EvaluateGraph directly; its existing phases are
			// processors and family kernels rather than the public EvaluateResult span.
			const std::string_view family =
				Graph.Workload == Fixture::Kind::Fft ? "imagegraph.node.audio" : "imagegraph.node.other";
			const auto observed = [&](std::string_view reason) {
				std::string message(reason);
				message += " expected=source_pipeline.sample,processor,";
				message += family;
				message += " observed=";
				std::array<std::string_view, 32> names{};
				size_t count = 0;
				for (const auto &span : FrameGraph::Spans()) {
					if (std::find(names.begin(), names.begin() + count, span.Name) != names.begin() + count)
						continue;
					if (count == names.size()) {
						message += "[truncated]";
						break;
					}
					names[count++] = span.Name;
					message += span.Name.substr(0, 64);
					message += ';';
				}
				return std::runtime_error(message);
			};
			bool processed = false, kernel = false;
			size_t owners = 0;
			for (size_t i = 0; i < FrameGraph::Spans().size(); ++i) {
				const auto &span = FrameGraph::Spans()[i];
				processed |= span.Name == "imagegraph.processor";
				kernel |= span.Name == family;
				if (span.Parent == FrameGraph::NO_PARENT) {
					++owners;
					if (span.Name != "imagegraph.source_pipeline.sample" || span.Depth != 0)
						throw observed("Source pipeline owner scope mismatch");
				}
				if (span.Reported ||
					(span.Parent != FrameGraph::NO_PARENT &&
					 (span.Parent >= i || span.Depth != FrameGraph::Spans()[span.Parent].Depth + 1)))
					throw observed("Source pipeline hierarchy is incomplete");
			}
			if (owners != 1 || !processed || !kernel)
				throw observed("Source pipeline evaluation phases absent");
			Reading reading;
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
					if (reading.Phases.size() == MaximumNames)
						throw std::runtime_error("Source pipeline phase names exceeded bound");
					reading.Phases.push_back({name});
					phase = std::prev(reading.Phases.end());
				}
				phase->SelfMilliseconds += span.SelfMilliseconds;
				++phase->Calls;
			}
			for (const auto &counter : Metrics::Drain()) {
				if (reading.Counters.size() == MaximumNames)
					throw std::runtime_error("Source pipeline counter names exceeded bound");
				reading.Counters.push_back({counter.Name, counter.Value, double(counter.Samples)});
			}
			Graph.Validate();
			engine::testing::Consume(Graph.Output());
			// Retain only bounded name tables and online means. Any harness sample count
			// uses constant recorder residency after the eight discarded warmups.
			if (++Calls <= 8) return;
			++Samples;
			Low = std::min(Low, reading.Milliseconds);
			High = std::max(High, reading.Milliseconds);
			Summary.UnmarkedMilliseconds =
				std::max(Summary.UnmarkedMilliseconds, reading.UnmarkedMilliseconds);
			const auto mean = [&](double &total, double next) { total += (next - total) / double(Samples); };
			mean(Summary.AllocatedBytes, reading.AllocatedBytes);
			mean(Summary.AllocatedBlocks, reading.AllocatedBlocks);
			mean(Summary.LiveDeltaBytes, reading.LiveDeltaBytes);
			Summary.ProcessPeakBytes = std::max(Summary.ProcessPeakBytes, reading.ProcessPeakBytes);
			Summary.ProfilerOverheadBytes =
				std::max(Summary.ProfilerOverheadBytes, reading.ProfilerOverheadBytes);
			for (const auto &p : reading.Phases) {
				auto found =
					std::find_if(Summary.Phases.begin(), Summary.Phases.end(), [&](const Phase &other) {
						return p.Name == other.Name;
					});
				if (found == Summary.Phases.end()) {
					if (Summary.Phases.size() == MaximumNames)
						throw std::runtime_error("Source pipeline aggregate phase bound");
					Summary.Phases.push_back({p.Name});
					found = std::prev(Summary.Phases.end());
				}
				mean(found->SelfMilliseconds, p.SelfMilliseconds);
				mean(found->Calls, p.Calls);
			}
			for (const auto &counter : reading.Counters) {
				auto found = std::find_if(
					Summary.Counters.begin(), Summary.Counters.end(), [&](const RecordedCounter &other) {
						return counter.Name == other.Name;
					}
				);
				if (found == Summary.Counters.end()) {
					if (Summary.Counters.size() == MaximumNames)
						throw std::runtime_error("Source pipeline aggregate counter bound");
					Summary.Counters.push_back({counter.Name});
					found = std::prev(Summary.Counters.end());
				}
				mean(found->Value, counter.Value);
				mean(found->Samples, counter.Samples);
			}
		}
		~ProfileFixture() {
			// Eight warmups are omitted. Frame time measures the workload; framework ns
			// also includes recording.
			if (!Samples) return;
			const size_t samples = Samples;
			const auto &phases = Summary.Phases;
			const auto &counters = Summary.Counters;
			const double low = Low, high = High, unmarked = Summary.UnmarkedMilliseconds;
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_PIPELINE_PRESET");
			std::printf(
				"# pipeline-profile preset=%s backend=cpu kind=%u nodes=%zu capture_seed=123456 "
				"work_units=%u input_fnv=%llu "
				"ticks=0..%llu samples=%zu frame_min_ms=%.6f frame_max_ms=%.6f unmarked_max_ms=%.6f "
				"known_output_fnv=%llu retained_output_bytes=%llu retained_data_replay_bytes=%llu "
				"heap_status=%s\n",
				preset ? preset : "unreported",
				unsigned(Graph.Workload),
				Graph.Authored.Nodes.size(),
				Graph.Workload == Fixture::Kind::Fft	? 256u
				: Graph.Workload == Fixture::Kind::Path ? 576u
														: 512u,
				static_cast<unsigned long long>(Graph.InputFingerprint),
				static_cast<unsigned long long>(
					Graph.Workload != Fixture::Kind::Strand ? 0 : Fixture::Frames - 1
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
					"# pipeline-heap kind=%u allocated_bytes_per_call=%llu allocated_blocks_per_call=%llu "
					"live_delta_bytes_per_call=%lld process_peak_bytes=%lld profiler_overhead_bytes=%lld\n",
					unsigned(Graph.Workload),
					static_cast<unsigned long long>(Summary.AllocatedBytes),
					static_cast<unsigned long long>(Summary.AllocatedBlocks),
					static_cast<long long>(Summary.LiveDeltaBytes),
					static_cast<long long>(Summary.ProcessPeakBytes),
					static_cast<long long>(Summary.ProfilerOverheadBytes)
				);

			for (const auto &p : phases)
				std::printf(
					"# pipeline-phase kind=%u name=%s calls_per_sample=%.0f "
					"self_mean_ms=%.6f\n",
					unsigned(Graph.Workload),
					p.Name.c_str(),
					p.Calls,
					p.SelfMilliseconds
				);
			for (const auto &counter : counters)
				std::printf(
					"# pipeline-counter kind=%u name=%.*s value_per_sample=%.0f "
					"observations_per_sample=%.0f\n",
					unsigned(Graph.Workload),
					int(counter.Name.Text().size()),
					counter.Name.Text().data(),
					counter.Value,
					counter.Samples
				);
		}
	};
} // namespace
BENCH("Source FFT N256 bins129 rows3 choices0.5,1.5,4294967297.5 tick0 compiled evaluation", 1) {
	static ProfileFixture f(Fixture::Kind::Fft);
	f.Measure();
}
BENCH(
	"Source Path anchors2 Transform+MapArea+Redistribute+Sample modes9 rows64-per-mode tick0 compiled "
	"evaluation",
	1
) {
	static ProfileFixture f(Fixture::Kind::Path);
	f.Measure();
}
BENCH("Source Strand hairs64 segments1 gravity1+2 step1 capture123456 ticks0..7 fresh replay", 1) {
	static ProfileFixture f(Fixture::Kind::Strand);
	f.Measure();
}
