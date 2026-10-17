// Build-stage costs and returned payload residency, with fixed diagnostic
// scopes. Analytical verification surrounds the actual measured hull owner.
#include "../tests/fixtures/ConvexHullParity.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
TEST_SUITE_ID("engine.collision.bench.convexhull")
namespace {
	using engine::core::FrameGraph;
	using engine::core::HeapNodeView;
	using engine::core::HeapProfile;
	constexpr std::array<std::string_view, 6> TAGS{
		"collision hull build",
		"collision hull weld",
		"collision hull seed",
		"collision hull expand",
		"collision hull remap",
		"collision hull faces"
	};
	struct Reading {
		std::array<HeapNodeView, TAGS.size()> Tags{};
		HeapNodeView Inclusive;
	};
	void Add(HeapNodeView &sum, const HeapNodeView &node) {
		sum.TotalBytes += node.TotalBytes;
		sum.TotalBlocks += node.TotalBlocks;
		sum.LiveBytes += node.LiveBytes;
		sum.LiveBlocks += node.LiveBlocks;
		sum.PeakBytes += node.PeakBytes;
	}
	Reading ReadHeap() {
		Reading result;
		const uint32_t count = HeapProfile::NodeCount();
		hull_fixture::Require(count <= HeapProfile::MAXIMUM_NODES, "hull heap nodes exceeded bound");
		uint32_t root = HeapProfile::ROOT;
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Name != TAGS.front() || node.Parent != HeapProfile::ROOT) continue;
			hull_fixture::Require(root == HeapProfile::ROOT, "hull ambiguous heap owner");
			root = index;
		}
		if (root == HeapProfile::ROOT) return result;
		for (uint32_t index = root; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			uint32_t at = index;
			size_t depth = 0;
			while (at != root && at != HeapProfile::ROOT) {
				hull_fixture::Require(++depth <= HeapProfile::MAXIMUM_DEPTH, "hull heap parent cycle");
				at = HeapProfile::Node(at).Parent;
			}
			if (at != root) continue;
			const auto tag = std::find(TAGS.begin(), TAGS.end(), node.Name);
			hull_fixture::Require(tag != TAGS.end(), "hull unexpected heap tag");
			Add(result.Tags[static_cast<size_t>(tag - TAGS.begin())], node);
			Add(result.Inclusive, node);
		}
		return result;
	}
	bool Enabled(const char *name) {
		const char *value = std::getenv(name);
		return value && std::string_view(value) == "1";
	}
	struct ProfileRestore {
		bool Enabled = FrameGraph::IsEnabled();
		~ProfileRestore() {
			FrameGraph::SetEnabled(Enabled);
		}
	};
	void VerifySpans(bool flat) {
		const auto spans = FrameGraph::Spans();
		hull_fixture::Require(
			FrameGraph::Dropped() == 0 && spans.size() == (flat ? 3 : 6), "hull incomplete owner frame"
		);
		std::array<size_t, 6> counts{};
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			const auto found = std::find(TAGS.begin(), TAGS.end(), span.Name);
			hull_fixture::Require(
				found != TAGS.end() && !span.Reported && span.IdleMilliseconds == 0 &&
					std::isfinite(span.Milliseconds) && std::isfinite(span.SelfMilliseconds) &&
					std::isfinite(span.StartMilliseconds) && span.Milliseconds >= 0 &&
					span.SelfMilliseconds >= 0,
				"hull invalid live span"
			);
			++counts[static_cast<size_t>(found - TAGS.begin())];
			hull_fixture::Require(
				index == 0 ? span.Name == TAGS[0] && span.Parent == FrameGraph::NO_PARENT && span.Depth == 0
						   : span.Parent == 0 && span.Depth == 1 &&
								 span.StartMilliseconds >= spans[0].StartMilliseconds &&
								 span.StartMilliseconds + span.Milliseconds <=
									 spans[0].StartMilliseconds + spans[0].Milliseconds + .001f,
				"hull phase hierarchy mismatch"
			);
			float children = 0;
			for (size_t child = index + 1; child < spans.size(); ++child)
				if (spans[child].Parent == index) children += spans[child].Milliseconds;
			hull_fixture::Require(
				std::abs(span.Milliseconds - span.SelfMilliseconds - children) <= .002f,
				"hull self inclusive mismatch"
			);
		}
		for (size_t phase = 0; phase < counts.size(); ++phase)
			hull_fixture::Require(
				counts[phase] == ((flat && phase >= 3) ? 0 : 1), "hull missing required phase"
			);
	}

	void PreflightOnce() {
		static const bool verified = [] {
			hull_fixture::Preflight(Enabled("ATOMIC_CONVEX_HULL_CANONICAL"));
			return true;
		}();
		(void)verified;
	}
	template <size_t ROW> void Run() {
		PreflightOnce();
		static const hull_fixture::Input input = hull_fixture::Make(ROW);
		static const bool capture = Enabled("ATOMIC_CONVEX_HULL_PROFILE");
		static const bool dump = Enabled("ATOMIC_CONVEX_HULL_CANONICAL");
		static size_t calls = 0;
		if (capture || dump)
			hull_fixture::Require(
				calls < 13, "hull diagnostics require eight warmups and at most five samples"
			);
		const uint64_t inputHash = hull_fixture::InputHash(input);
		const ProfileRestore restore;
		Reading before, retained, released;
		engine::core::HeapTotals processBefore, processRetained, processReleased;
		if (capture) {
			hull_fixture::Require(HeapProfile::IsCompiledIn(), "hull capture requires heap hooks");
			FrameGraph::SetEnabled(true);
			FrameGraph::BeginFrame();
			before = ReadHeap();
			processBefore = HeapProfile::Totals();
		}
		uint64_t outputHash = 0;
		size_t payload = 0, capacity = 0, returnedBlocks = 0;
		try {
			// The returned value lives through verification/export, just as in the
			// uncaptured path. Heap reads allocate no snapshots or canonical words.
			const auto hull = engine::collision::BuildConvexHull(input.Points, input.Tolerance);
			if (capture) {
				retained = ReadHeap();
				processRetained = HeapProfile::Totals();
			}
			payload = hull.Points.size() * sizeof(hull.Points.front()) +
					  hull.Faces.size() * sizeof(engine::collision::HullFace) +
					  hull.Loops.size() * sizeof(uint32_t);
			capacity = hull.Points.capacity() * sizeof(engine::core::Vector3) +
					   hull.Faces.capacity() * sizeof(engine::collision::HullFace) +
					   hull.Loops.capacity() * sizeof(uint32_t);
			returnedBlocks = static_cast<size_t>(hull.Points.capacity() != 0) +
							 static_cast<size_t>(hull.Faces.capacity() != 0) +
							 static_cast<size_t>(hull.Loops.capacity() != 0);
			hull_fixture::Verify(input, hull);
			const auto words = hull_fixture::Canonical(hull);
			outputHash = hull_fixture::Hash(words);
			if (dump) {
				std::printf(
					"# hull-canonical row=%zu call=%zu BENCH_timings_invalid=1 input_hash=%016" PRIx64
					" words=",
					ROW,
					calls + 1,
					inputHash
				);
				for (uint32_t word : words)
					std::printf("%08x", word);
				std::printf("\n");
			}
			engine::testing::Consume(hull.Points.size());
		} catch (...) {
			// Close the owner frame on refusal before restoring capture state.
			if (capture) FrameGraph::EndFrame();
			throw;
		}
		if (capture) {
			released = ReadHeap();
			processReleased = HeapProfile::Totals();
			FrameGraph::EndFrame();
			VerifySpans(input.Flat);
			hull_fixture::Require(
				processRetained.DroppedScopes == processBefore.DroppedScopes &&
					processReleased.DroppedScopes == processBefore.DroppedScopes &&
					retained.Inclusive.TotalBytes - before.Inclusive.TotalBytes ==
						processRetained.TotalBytes - processBefore.TotalBytes &&
					retained.Inclusive.TotalBlocks - before.Inclusive.TotalBlocks ==
						processRetained.TotalBlocks - processBefore.TotalBlocks,
				"hull allocation attribution failed"
			);
			hull_fixture::Require(
				retained.Inclusive.LiveBytes - released.Inclusive.LiveBytes ==
						static_cast<int64_t>(capacity) &&
					retained.Inclusive.LiveBlocks - released.Inclusive.LiveBlocks ==
						static_cast<int64_t>(returnedBlocks),
				"hull returned capacity release ledger failed"
			);
		}
		hull_fixture::Require(hull_fixture::InputHash(input) == inputHash, "hull mutated authored cloud");
		++calls;
		if (!capture) return;
		const auto spans = FrameGraph::Spans();
		std::printf(
			"# hull-profile row=%zu call=%zu warmup=%d input_points=%zu tolerance=%.8f weld=%.8f frames=1 "
			"spans=%zu frame_drops=0 heap_drop_delta=0 reconciled=1 "
			"heap_coverage=cxx_new_delete inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f frame_ms=%.6f "
			"unmarked_ms=%.6f input_hash=%016" PRIx64 " output_hash=%016" PRIx64 " allocated_bytes=%" PRIu64
			" allocated_blocks=%" PRIu64 " live_bytes_before=%" PRId64 " live_bytes_retained=%" PRId64
			" live_bytes_released=%" PRId64 " live_blocks_before=%" PRId64 " live_blocks_retained=%" PRId64
			" live_blocks_released=%" PRId64 " sum_tag_peak_bytes=%" PRId64
			" returned_payload_bytes=%zu returned_capacity_bytes=%zu returned_capacity_blocks=%zu "
			"profiler_overhead_bytes=%" PRId64 "\n",
			ROW,
			calls,
			calls <= 8,
			input.Points.size(),
			input.Tolerance,
			engine::collision::HULL_WELD_DISTANCE,
			spans.size(),
			spans[0].Milliseconds,
			spans[0].SelfMilliseconds,
			spans[0].IdleMilliseconds,
			FrameGraph::FrameMilliseconds(),
			FrameGraph::UnmarkedMilliseconds(),
			inputHash,
			outputHash,
			retained.Inclusive.TotalBytes - before.Inclusive.TotalBytes,
			retained.Inclusive.TotalBlocks - before.Inclusive.TotalBlocks,
			before.Inclusive.LiveBytes,
			retained.Inclusive.LiveBytes,
			released.Inclusive.LiveBytes,
			before.Inclusive.LiveBlocks,
			retained.Inclusive.LiveBlocks,
			released.Inclusive.LiveBlocks,
			retained.Inclusive.PeakBytes,
			payload,
			capacity,
			returnedBlocks,
			processReleased.OverheadBytes
		);
		for (size_t tag = 0; tag < TAGS.size(); ++tag) {
			float inclusive = 0, self = 0;
			for (const auto &span : spans)
				if (span.Name == TAGS[tag]) {
					inclusive += span.Milliseconds;
					self += span.SelfMilliseconds;
				}
			std::printf(
				"# hull-phase row=%zu call=%zu tag=%zu inclusive_ms=%.6f self_ms=%.6f "
				"exclusive_bytes=%" PRIu64 " exclusive_blocks=%" PRIu64 " live_bytes_before=%" PRId64
				" live_bytes_retained=%" PRId64 " live_bytes_released=%" PRId64 " live_blocks_before=%" PRId64
				" live_blocks_retained=%" PRId64 " live_blocks_released=%" PRId64 "\n",
				ROW,
				calls,
				tag,
				inclusive,
				self,
				retained.Tags[tag].TotalBytes - before.Tags[tag].TotalBytes,
				retained.Tags[tag].TotalBlocks - before.Tags[tag].TotalBlocks,
				before.Tags[tag].LiveBytes,
				retained.Tags[tag].LiveBytes,
				released.Tags[tag].LiveBytes,
				before.Tags[tag].LiveBlocks,
				retained.Tags[tag].LiveBlocks,
				released.Tags[tag].LiveBlocks
			);
		}
	}
}
BENCH("Hull box 1024 interior", 1) {
	Run<0>();
}
BENCH("Hull box 30000 interior plus seam duplicates", 1) {
	Run<1>();
}
BENCH("Hull octagonal prism 1024 interior", 1) {
	Run<2>();
}
BENCH("Hull octagonal prism weld-cell seams", 1) {
	Run<3>();
}
BENCH("Hull flat64 points plus duplicate and nonfinite", 1) {
	Run<4>();
}
BENCH("Hull128 exposed paraboloid points capped", 1) {
	Run<5>();
}
