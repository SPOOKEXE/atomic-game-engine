#include "../tests/fixtures/SourcePixelKernels.hpp"

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

TEST_SUITE_ID("engine.imagegraph.bench.pixel-kernels")
namespace {
	using Fixture = engine::imagegraph::testing::SourcePixelKernelFixture;
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
	struct Profile {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		explicit Profile(Fixture::Kind kind) : Graph(kind) {}
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
			size_t roots = 0, evaluations = 0, processors = 0;
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
				if (span.Name == "imagegraph.processor") {
					if (span.Parent == FrameGraph::NO_PARENT || span.Depth < 2)
						Fixture::Fail("pixel kernel processor must belong to the evaluation hierarchy");
					++processors;
				}
				// Recorder copies and analytical verification stay outside the captured frame and heap
				// interval.
				reading.Spans.push_back({std::string(span.Name), span});
			}
			if (roots != 1 || evaluations != 1 || processors == 0)
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
					unsigned(Graph.Workload),
					i + 1,
					i < 8,
					Graph.Authored.Nodes.size(),
					Graph.OutputSide(),
					Graph.OutputFormat(),
					Graph.IsPalette() ? 512u : 0u,
					(Graph.Workload == Fixture::Kind::GaussianRandom ||
					 Graph.Workload == Fixture::Kind::AnisoBlend ||
					 Graph.Workload == Fixture::Kind::AnisoMapped)
						? 17.25
						: 0.0,
					(Graph.Workload == Fixture::Kind::FoldGreyscale ||
					 Graph.Workload == Fixture::Kind::FoldMap || Graph.IsFieldSample())
						? 3u
						: 0u,
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
						unsigned(Graph.Workload),
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
						unsigned(Graph.Workload),
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
BENCH("CPU Atlas Draw 128x128 16 ordered sprites compiled-plan", 1) {
	static Profile p(Fixture::Kind::Atlas16);
	p.Measure();
}
BENCH("CPU Atlas Draw 128x128 64 ordered sprites compiled-plan", 1) {
	static Profile p(Fixture::Kind::Atlas64);
	p.Measure();
}
BENCH("CPU Palette Sort 512 colours RGB packed key compiled-plan", 1) {
	static Profile p(Fixture::Kind::PaletteRGB);
	p.Measure();
}
BENCH("CPU Palette Sort 512 colours reverse custom key compiled-plan", 1) {
	static Profile p(Fixture::Kind::PaletteReverse);
	p.Measure();
}
BENCH("CPU Edge Detect 128x128 Sobel 9 neighbors compiled-plan", 1) {
	static Profile p(Fixture::Kind::EdgeSobel);
	p.Measure();
}
BENCH("CPU Edge Detect 128x128 Laplacian 9 neighbors compiled-plan", 1) {
	static Profile p(Fixture::Kind::EdgeLaplacian);
	p.Measure();
}
BENCH("CPU Bokeh 128x128 strength8 taps8 compiled-plan", 1) {
	static Profile p(Fixture::Kind::Bokeh8);
	p.Measure();
}
BENCH("CPU Bokeh 128x128 strength8 taps32 compiled-plan", 1) {
	static Profile p(Fixture::Kind::Bokeh32);
	p.Measure();
}

BENCH("CPU Fold Greyscale 64x64 iteration3 compiled-plan", 1) {
	static Profile p(Fixture::Kind::FoldGreyscale);
	p.Measure();
}

BENCH("CPU Fold Map 64x64 iteration3 compiled-plan", 1) {
	static Profile p(Fixture::Kind::FoldMap);
	p.Measure();
}

BENCH("CPU Gaussian random 64x64 seed17.25 compiled-plan", 1) {
	static Profile p(Fixture::Kind::GaussianRandom);
	p.Measure();
}

BENCH("CPU Gaussian conversion 64x64 two red samplers compiled-plan", 1) {
	static Profile p(Fixture::Kind::GaussianConversion);
	p.Measure();
}

BENCH("CPU Aniso Blend 64x64 two seeds compiled-plan", 1) {
	static Profile p(Fixture::Kind::AnisoBlend);
	p.Measure();
}

BENCH("CPU Aniso Waterfall 64x64 three mapped controls tile compiled-plan", 1) {
	static Profile p(Fixture::Kind::AnisoMapped);
	p.Measure();
}

BENCH("CPU Fold RGB field 32x32 Sample Vector3 compiled-plan", 1) {
	static Profile p(Fixture::Kind::RasterRGB);
	p.Measure();
}
