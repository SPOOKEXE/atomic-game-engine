#pragma once
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	struct SpanRecord {
		std::string Name;
		engine::core::FrameSpan Span;
	};
	struct Reading {
		float Milliseconds = 0, UnmarkedMilliseconds = 0;
		engine::core::HeapTotals Before{}, After{};
		std::vector<SpanRecord> Spans;
		std::vector<engine::core::Counter> Counters;
	};
	template <class Fixture> struct Profile {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		explicit Profile(typename Fixture::Kind kind) : Graph(kind) {}
		void Measure() {
			if (Count == Readings.size()) Fixture::Fail("pixel kernel profiles support one to five samples");
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			FrameGraph::SetEnabled(true);
			Metrics::Drain();
			auto &reading = Readings[Count];
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraph.pixel_kernel.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.UnmarkedMilliseconds = FrameGraph::UnmarkedMilliseconds();
			if (FrameGraph::Dropped() || reading.Before.DroppedScopes != reading.After.DroppedScopes)
				Fixture::Fail("pixel kernel profile dropped scopes");
			const auto &spans = FrameGraph::Spans();
			size_t roots = 0, evaluations = 0, processors = 0, work = 0;
			for (size_t index = 0; index < spans.size(); ++index) {
				const auto &span = spans[index];
				if (span.Reported) Fixture::Fail("pixel kernel CPU profile has an unexpected reported span");
				if (span.Parent == FrameGraph::NO_PARENT) {
					if (span.Depth != 0 || span.Name != "imagegraph.pixel_kernel.sample")
						Fixture::Fail("pixel kernel profile has an unexpected root");
					++roots;
				} else if (span.Parent >= index || span.Depth != spans[span.Parent].Depth + 1)
					Fixture::Fail("pixel kernel profile has a malformed parent/depth hierarchy");
				if (span.Name == "imagegraph.evaluate") {
					if (span.Parent != 0 || span.Depth != 1)
						Fixture::Fail(
							"pixel kernel evaluation must be directly owned by its complete sample"
						);
					++evaluations;
				}
				if (span.Name == Graph.ProfileWorkScope()) ++work;
				if (span.Name == "imagegraph.processor") {
					if (span.Parent == FrameGraph::NO_PARENT || span.Depth < 2)
						Fixture::Fail("pixel kernel processor must belong to the evaluation hierarchy");
					++processors;
				}
				// Recorder copies and analytical verification stay outside the captured frame and heap
				// interval.
				reading.Spans.push_back({std::string(span.Name), span});
			}
			if (roots != 1 || evaluations != Graph.ProfileEvaluations() ||
				(Graph.ProfileProcessors() && processors == 0) || work == 0)
				Fixture::Fail("pixel kernel profile missed its complete evaluation or processor work");
			reading.Counters = Metrics::Drain();
			if (Graph.Verify() != Graph.ExpectedHash) Fixture::Fail("measured output changed");
			++Count;
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_PIXEL_PRESET");
			for (size_t i = 0; i < Count; ++i) {
				const auto &reading = Readings[i];
				std::printf(
					"# pixel-profile preset=%s backend=cpu kind=%u call=%zu warmup=%d nodes=%zu side=%u "
					"format=%s "
					"palette_colours=%u tick=0 seed=%.2f iteration=%u output_fnv=%llu owner_ms=%.6f "
					"unmarked_ms=%.6f "
					"heap_compiled=%d "
					"allocated_bytes=%llu allocated_blocks=%llu process_live_bytes=%lld "
					"process_peak_bytes=%lld "
					"overhead_bytes=%lld\n",
					preset ? preset : "unreported",
					Graph.ProfileKind(),
					i + 1,
					i < 8,
					Graph.Authored.Nodes.size(),
					Graph.OutputSide(),
					Graph.OutputFormat(),
					Graph.IsPalette() ? 512u : 0u,
					Graph.ProfileSeed(),
					Graph.ProfileIterations(),
					static_cast<unsigned long long>(Graph.ExpectedHash),
					reading.Milliseconds,
					reading.UnmarkedMilliseconds,
					HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(reading.After.TotalBytes - reading.Before.TotalBytes),
					static_cast<unsigned long long>(reading.After.TotalBlocks - reading.Before.TotalBlocks),
					static_cast<long long>(reading.After.LiveBytes),
					static_cast<long long>(reading.After.PeakBytes),
					static_cast<long long>(reading.After.OverheadBytes)
				);
				for (size_t index = 0; index < reading.Spans.size(); ++index) {
					const auto &span = reading.Spans[index];
					std::printf(
						"# pixel-span kind=%u call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
						"inclusive_ms=%.6f "
						"self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						Graph.ProfileKind(),
						i + 1,
						index,
						span.Span.Parent,
						span.Span.Depth,
						span.Span.StartMilliseconds,
						span.Span.Milliseconds,
						span.Span.SelfMilliseconds,
						span.Span.IdleMilliseconds,
						span.Span.Reported,
						span.Name.c_str()
					);
				}
				for (const auto &counter : reading.Counters)
					std::printf(
						"# pixel-counter kind=%u call=%zu name=%.*s value=%.0f samples=%u\n",
						Graph.ProfileKind(),
						i + 1,
						int(counter.Name.Text().size()),
						counter.Name.Text().data(),
						counter.Value,
						counter.Samples
					);
			}
		}
	};
}
