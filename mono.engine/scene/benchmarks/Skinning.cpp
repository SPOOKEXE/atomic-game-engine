// Real headless pose resolution; authored preparation and full verification
// surround the measured resolver owner. Capture timings and heap hooks are
// diagnostic costs, not a claim about a shipped heap-hook-free build.
#include "../tests/fixtures/SkinningParity.hpp"

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

TEST_SUITE_ID("engine.scene.bench.skinning")

namespace {
	using engine::core::FrameGraph;
	using engine::core::HeapNodeView;
	using engine::core::HeapProfile;
	constexpr std::array<std::string_view, 7> TAGS{
		"scene resolve bones",
		"scene skinning rig gather",
		"scene skinning rig",
		"scene skinning joint gather",
		"scene skinning slot sort",
		"scene skinning pose compose",
		"scene skinning pose scratch"
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
		skinning_fixture::Require(count <= HeapProfile::MAXIMUM_NODES, "skinning heap nodes exceeded bound");
		uint32_t root = HeapProfile::ROOT;
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Name != TAGS.front() || node.Parent != HeapProfile::ROOT) continue;
			skinning_fixture::Require(root == HeapProfile::ROOT, "skinning ambiguous heap owner");
			root = index;
		}
		if (root == HeapProfile::ROOT) return result;
		for (uint32_t index = root; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			uint32_t at = index;
			size_t depth = 0;
			while (at != root && at != HeapProfile::ROOT) {
				skinning_fixture::Require(
					++depth <= HeapProfile::MAXIMUM_DEPTH, "skinning heap parent cycle"
				);
				at = HeapProfile::Node(at).Parent;
			}
			if (at != root) continue;
			const auto tag = std::find(TAGS.begin(), TAGS.end(), node.Name);
			skinning_fixture::Require(tag != TAGS.end(), "skinning unexpected heap tag");
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
	void VerifySpans(size_t rigs) {
		const auto spans = FrameGraph::Spans();
		skinning_fixture::Require(
			FrameGraph::Dropped() == 0 && spans.size() == 2 + rigs * 4, "skinning incomplete owner frame"
		);
		std::array<size_t, 6> counts{};
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			const auto found = std::find(TAGS.begin(), TAGS.begin() + 6, span.Name);
			skinning_fixture::Require(
				found != TAGS.begin() + 6 && !span.Reported && span.IdleMilliseconds == 0 &&
					std::isfinite(span.Milliseconds) && std::isfinite(span.SelfMilliseconds) &&
					span.Milliseconds >= 0 && span.SelfMilliseconds >= 0 &&
					std::isfinite(span.StartMilliseconds),
				"skinning invalid live span"
			);
			++counts[static_cast<size_t>(found - TAGS.begin())];
			if (index == 0) {
				skinning_fixture::Require(
					span.Name == TAGS.front() && span.Parent == FrameGraph::NO_PARENT && span.Depth == 0,
					"skinning missing whole resolver owner"
				);
			} else {
				skinning_fixture::Require(span.Parent < index, "skinning invalid span parent");
				const auto &parent = spans[span.Parent];
				skinning_fixture::Require(
					span.Depth == parent.Depth + 1 && span.StartMilliseconds >= parent.StartMilliseconds &&
						span.StartMilliseconds + span.Milliseconds <=
							parent.StartMilliseconds + parent.Milliseconds + .001f,
					"skinning span outside parent"
				);
				const bool childOfOwner = span.Name == TAGS[1] || span.Name == TAGS[2];
				skinning_fixture::Require(
					parent.Name == (childOfOwner ? TAGS[0] : TAGS[2]), "skinning unexpected phase hierarchy"
				);
			}
			float children = 0;
			for (size_t child = index + 1; child < spans.size(); ++child)
				if (spans[child].Parent == index) children += spans[child].Milliseconds;
			skinning_fixture::Require(
				std::abs(span.Milliseconds - span.SelfMilliseconds - children) <= .002f,
				"skinning self inclusive mismatch"
			);
		}
		skinning_fixture::Require(
			counts[0] == 1 && counts[1] == 1 && counts[2] == rigs && counts[3] == rigs && counts[4] == rigs &&
				counts[5] == rigs,
			"skinning missing evaluation phase"
		);
	}
	void Preflight() {
		skinning_fixture::World large(2, 256, true);
		skinning_fixture::World small(1, 32, false);
		for (size_t clock = 1; clock <= 8; ++clock) {
			large.Clock = clock;
			large.Prepare();
			const uint64_t authored = large.InputHash();
			skinning_fixture::Require(
				engine::scene::ResolveBones(large.Storage) == large.ExpectedWrites,
				"skinning preflight changed write count"
			);
			large.Verify();
			skinning_fixture::Require(large.InputHash() == authored, "skinning preflight authored mutation");
			const uint64_t version = large.Storage.ChangeVersion();
			skinning_fixture::Require(
				engine::scene::ResolveBones(large.Storage) == 0 && large.Storage.ChangeVersion() == version,
				"skinning preflight static version changed"
			);
			skinning_fixture::Require(
				engine::scene::ResolveBones(small.Storage) == 0, "skinning world switch changed stable pose"
			);
			small.Verify();
		}
	}
	template <size_t ROW, size_t RIGS, size_t JOINTS, bool ANIMATED> void Run() {
		static const bool checked = [] {
			Preflight();
			return true;
		}();
		(void)checked;
		static skinning_fixture::World world(RIGS, JOINTS, ANIMATED);
		static size_t calls = 0;
		static const bool capture = Enabled("ATOMIC_SKINNING_PROFILE");
		static const bool dump = Enabled("ATOMIC_SKINNING_CANONICAL");
		if (capture || dump)
			skinning_fixture::Require(
				calls < 13, "skinning diagnostics support eight warmups and at most five samples"
			);
		if constexpr (ANIMATED) {
			++world.Clock;
			world.Prepare();
		}
		const uint64_t inputHash = world.InputHash();
		const uint64_t version = world.Storage.ChangeVersion();
		size_t written = 0;
		Reading before, after;
		engine::core::HeapTotals processBefore, processAfter;
		const ProfileRestore restore;
		if (capture) {
			skinning_fixture::Require(HeapProfile::IsCompiledIn(), "skinning capture needs heap hooks");
			FrameGraph::SetEnabled(true);
			FrameGraph::BeginFrame();
			// Reads use stack aggregates, with no snapshot-vector allocation.
			before = ReadHeap();
			processBefore = HeapProfile::Totals();
		}
		try {
			written = engine::scene::ResolveBones(world.Storage);
		} catch (...) {
			// Close the owner frame on refusal before restoring capture state.
			if (capture) FrameGraph::EndFrame();
			throw;
		}
		if (capture) {
			after = ReadHeap();
			processAfter = HeapProfile::Totals();
			FrameGraph::EndFrame();
			VerifySpans(RIGS);
			skinning_fixture::Require(
				processAfter.DroppedScopes == processBefore.DroppedScopes &&
					after.Inclusive.TotalBytes - before.Inclusive.TotalBytes ==
						processAfter.TotalBytes - processBefore.TotalBytes &&
					after.Inclusive.TotalBlocks - before.Inclusive.TotalBlocks ==
						processAfter.TotalBlocks - processBefore.TotalBlocks,
				"skinning incomplete heap attribution"
			);
		}
		world.Verify();
		skinning_fixture::Require(world.InputHash() == inputHash, "skinning resolver mutated authored input");
		skinning_fixture::Require(
			written == (ANIMATED ? world.ExpectedWrites : 0), "skinning changed write count"
		);
		if constexpr (!ANIMATED)
			skinning_fixture::Require(
				world.Storage.ChangeVersion() == version, "skinning stable pass changed version"
			);
		++calls;
		if (capture) {
			const auto spans = FrameGraph::Spans();
			std::printf(
				"# skinning-profile row=%zu call=%zu warmup=%d rigs=%zu joints_per_rig=%zu animated=%d "
				"clock=%zu "
				"frames=1 spans=%zu frame_drops=0 heap_drop_delta=0 reconciled=1 "
				"heap_coverage=cxx_new_delete inclusive_ms=%.6f "
				"self_ms=%.6f idle_ms=%.6f "
				"frame_ms=%.6f unmarked_ms=%.6f input_hash=%016" PRIx64 " output_hash=%016" PRIx64
				" written=%zu allocated_bytes=%" PRIu64 " allocated_blocks=%" PRIu64
				" live_bytes_before=%" PRId64 " live_bytes_after=%" PRId64 " live_blocks_before=%" PRId64
				" live_blocks_after=%" PRId64 " sum_tag_peak_bytes=%" PRId64
				" profiler_overhead_bytes=%" PRId64 "\n",
				ROW,
				calls,
				calls <= 8,
				RIGS,
				JOINTS,
				ANIMATED,
				world.Clock,
				spans.size(),
				spans[0].Milliseconds,
				spans[0].SelfMilliseconds,
				spans[0].IdleMilliseconds,
				FrameGraph::FrameMilliseconds(),
				FrameGraph::UnmarkedMilliseconds(),
				inputHash,
				world.OutputHash(),
				written,
				after.Inclusive.TotalBytes - before.Inclusive.TotalBytes,
				after.Inclusive.TotalBlocks - before.Inclusive.TotalBlocks,
				before.Inclusive.LiveBytes,
				after.Inclusive.LiveBytes,
				before.Inclusive.LiveBlocks,
				after.Inclusive.LiveBlocks,
				after.Inclusive.PeakBytes,
				processAfter.OverheadBytes
			);
			for (size_t tag = 0; tag < TAGS.size(); ++tag) {
				float inclusive = 0, self = 0;
				for (const auto &span : spans)
					if (span.Name == TAGS[tag]) {
						inclusive += span.Milliseconds;
						self += span.SelfMilliseconds;
					}
				std::printf(
					"# skinning-phase row=%zu call=%zu tag=%zu inclusive_ms=%.6f self_ms=%.6f "
					"exclusive_bytes=%" PRIu64 " exclusive_blocks=%" PRIu64 " live_bytes_before=%" PRId64
					" live_bytes_after=%" PRId64 "\n",
					ROW,
					calls,
					tag,
					inclusive,
					self,
					after.Tags[tag].TotalBytes - before.Tags[tag].TotalBytes,
					after.Tags[tag].TotalBlocks - before.Tags[tag].TotalBlocks,
					before.Tags[tag].LiveBytes,
					after.Tags[tag].LiveBytes
				);
			}
		}
		if (dump) {
			std::printf("# skinning-correctness-only BENCH_timings_invalid=1\n");
			world.Dump(ROW, calls);
		}
		engine::testing::Consume(written);
	}
}
BENCH("ResolveBones 1x32 static", 1) {
	Run<0, 1, 32, false>();
}
BENCH("ResolveBones 1x32 animated", 1) {
	Run<1, 1, 32, true>();
}
BENCH("ResolveBones 32x64 static", 1) {
	Run<2, 32, 64, false>();
}
BENCH("ResolveBones 32x64 animated", 1) {
	Run<3, 32, 64, true>();
}
BENCH("ResolveBones 16x256 static", 1) {
	Run<4, 16, 256, false>();
}
BENCH("ResolveBones 16x256 animated", 1) {
	Run<5, 16, 256, true>();
}
