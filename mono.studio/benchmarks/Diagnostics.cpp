// Structural averaging for a granular frame graph.

#include <engine/core/FrameGraph.hpp>
#include <engine/testing/Bench.hpp>

#include <studio/Diagnostics.hpp>
#include <vector>

TEST_SUITE_ID("studio.bench.diagnostics")

namespace {
	std::vector<engine::core::FrameSpan> &GranularFrame() {
		static std::vector<engine::core::FrameSpan> frame = [] {
			std::vector<engine::core::FrameSpan> spans;
			spans.reserve(4'097);
			spans.push_back(
				engine::core::FrameSpan{
					.Name = "phase",
					.Parent = engine::core::FrameGraph::NO_PARENT,
					.Milliseconds = 4.0f,
				}
			);
			for (uint32_t index = 0; index < 4'096; index++) {
				spans.push_back(
					engine::core::FrameSpan{
						.Name = "system",
						.Depth = 1,
						.Parent = 0,
						.StartMilliseconds = static_cast<float>(index) / 1024.0f,
						.Milliseconds = 1.0f / 1024.0f,
					}
				);
			}
			return spans;
		}();
		return frame;
	}
}

BENCH_PER_ITEM("average 4096 repeated system spans", 100) {
	std::vector<studio::DiagnosticSpan> totals;
	studio::AccumulateDiagnosticSpans(GranularFrame(), totals);

	for (uint32_t frame = 0; frame < 100; frame++) {
		studio::AccumulateDiagnosticSpans(GranularFrame(), totals);
		engine::testing::Consume(totals.size());
	}
}
