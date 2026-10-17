// The cost of resolving authored lighting once per presented world and frame.
//
// The resolver walks the small root service set, evaluates the solar arc, and
// copies a value into the renderer. This row keeps that fixed cost visible as
// worlds are added to one client.

#include "../tests/fixtures/SunlightParity.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Bench.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

TEST_SUITE_ID("engine.scene.bench.sunlight")

namespace {
	engine::ecs::Store &World() {
		static engine::ecs::Store store("bench.sunlight");
		static const bool ready = [] {
			engine::scene::RegisterSceneClasses();
			engine::scene::InstallServices(store);
			return true;
		}();
		(void)ready;
		return store;
	}
	constexpr std::string_view BATCH_SCOPE = "scene sunlight batch";

	constexpr std::string_view LOOKUP_SCOPE = "scene service lookup";
	constexpr std::string_view ROOT_SCOPE = "ecs root snapshot";
	struct AllocationReadings {
		engine::core::HeapNodeView Inclusive;
		engine::core::HeapNodeView Batch;
		engine::core::HeapNodeView Lookup;
		engine::core::HeapNodeView Roots;
		size_t Nodes = 0;
	};
	void Add(engine::core::HeapNodeView &total, const engine::core::HeapNodeView &node) {
		total.TotalBytes += node.TotalBytes;
		total.TotalBlocks += node.TotalBlocks;
		total.LiveBytes += node.LiveBytes;
		total.LiveBlocks += node.LiveBlocks;
		total.PeakBytes += node.PeakBytes;
	}
	AllocationReadings BatchHeap() {
		using engine::core::HeapProfile;
		const uint32_t count = HeapProfile::NodeCount();
		if (count > 4096) throw std::runtime_error("sunlight heap tree exceeds diagnostic bound");
		uint32_t batch = HeapProfile::ROOT;
		for (uint32_t index = 1; index < count; ++index) {
			if (HeapProfile::Node(index).Name != BATCH_SCOPE) continue;
			if (batch != HeapProfile::ROOT)
				throw std::runtime_error("sunlight batch has ambiguous heap paths");
			batch = index;
		}
		AllocationReadings result;
		if (batch == HeapProfile::ROOT) return result;
		for (uint32_t index = batch; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			uint32_t parent = index;
			size_t depth = 0;
			while (parent != batch && parent != HeapProfile::ROOT) {
				if (++depth > HeapProfile::MAXIMUM_DEPTH)
					throw std::runtime_error("sunlight heap hierarchy invalid");
				parent = HeapProfile::Node(parent).Parent;
			}
			if (parent != batch) continue;
			// Node counters are exclusive. Sum each descendant exactly once to
			// reconcile the whole batch without double-counting nested lookups.
			Add(result.Inclusive, node);
			++result.Nodes;
			if (index == batch)
				Add(result.Batch, node);
			else if (node.Name == LOOKUP_SCOPE && node.Parent == batch)
				Add(result.Lookup, node);
			else if (node.Name == ROOT_SCOPE && HeapProfile::Node(node.Parent).Name == LOOKUP_SCOPE &&
					 HeapProfile::Node(node.Parent).Parent == batch)
				Add(result.Roots, node);
			else
				throw std::runtime_error("sunlight batch has unexpected heap descendants");
		}
		return result;
	}

	struct RestoreProfile {
		bool Enabled = engine::core::FrameGraph::IsEnabled();
		~RestoreProfile() {
			engine::core::FrameGraph::SetEnabled(Enabled);
		}
	};
}

BENCH_PER_ITEM("LightingOf · furnished world", 100'000) {
	static const bool verified = [] {
		sunlight_fixture::Verify();
		return true;
	}();
	(void)verified;
	engine::ecs::Store &store = World();
	const engine::ecs::Entity service = store.FindFirstRoot("Lighting");
	auto *authored = store.GetMutable<engine::scene::LightingServiceComponent>(service);

	const auto run = [&] {
		for (uint32_t frame = 0; frame < 100'000; frame++) {
			// Vary a real input so the optimiser cannot hoist one immutable answer
			// out of the measured frame loop.
			authored->ClockTime = static_cast<float>(frame % 24u);
			engine::testing::Consume(engine::scene::LightingOf(store));
		}
	};
	static const bool capture = [] {
		const char *value = std::getenv("ATOMIC_SUNLIGHT_PROFILE");
		return value != nullptr && std::string_view(value) == "1";
	}();
	if (!capture) {
		run();
		return;
	}
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	static size_t calls = 0;
	if (!HeapProfile::IsCompiledIn() || calls == 128)
		throw std::runtime_error("sunlight capture requires heap hooks and at most 128 batches");
	const RestoreProfile restore;
	FrameGraph::SetEnabled(true);
	const auto beforeReadings = BatchHeap();
	const auto &before = beforeReadings.Inclusive;
	const auto totalsBefore = HeapProfile::Totals();
	FrameGraph::BeginFrame();
	try {
		// One owner batch includes every resolver call. This serial workload has
		// no worker join and no per-name timing or reconstructed producer spans.
		ENGINE_PROFILE("scene sunlight batch");
		run();
	} catch (...) {
		// Close the owner frame on refusal before restoring collector state.
		FrameGraph::EndFrame();
		throw;
	}
	FrameGraph::EndFrame();
	// Consume this completed frame immediately; retained history is not used
	// as evidence that every submitted batch survived its bounded ring.
	const auto &spans = FrameGraph::Spans();
	if (FrameGraph::Dropped() != 0 || spans.size() != 1 || spans.front().Name != BATCH_SCOPE ||
		spans.front().Depth != 0 || spans.front().Parent != FrameGraph::NO_PARENT || spans.front().Reported)
		throw std::runtime_error("sunlight capture incomplete hierarchy or dropped spans");
	const auto afterReadings = BatchHeap();
	const auto &after = afterReadings.Inclusive;
	if ((afterReadings.Nodes != 2 && afterReadings.Nodes != 3) ||
		after.TotalBytes != afterReadings.Batch.TotalBytes + afterReadings.Lookup.TotalBytes +
								afterReadings.Roots.TotalBytes ||
		after.TotalBlocks != afterReadings.Batch.TotalBlocks + afterReadings.Lookup.TotalBlocks +
								 afterReadings.Roots.TotalBlocks)
		throw std::runtime_error("sunlight heap allocation attribution did not reconcile");
	const auto totalsAfter = HeapProfile::Totals();
	if (totalsAfter.DroppedScopes != totalsBefore.DroppedScopes)
		throw std::runtime_error("sunlight capture dropped heap scopes");
	++calls;
	std::printf(
		"# sunlight-profile call=%zu warmup=%d resolves=100000 frames=1 spans=1 frame_drops=0 "
		"heap_drop_delta=0 inclusive_ms=%.6f self_ms=%.6f frame_ms=%.6f unmarked_ms=%.6f "
		"heap_coverage=cxx_new_delete allocated_bytes=%" PRIu64 " allocated_blocks=%" PRIu64
		" batch_exclusive_bytes=%" PRIu64 " batch_exclusive_blocks=%" PRIu64
		" lookup_exclusive_bytes=%" PRIu64 " lookup_exclusive_blocks=%" PRIu64
		" roots_exclusive_bytes=%" PRIu64 " roots_exclusive_blocks=%" PRIu64
		" heap_nodes=%zu attribution_reconciled=1 live_bytes_before=%" PRId64 " live_bytes_after=%" PRId64
		" live_blocks_before=%" PRId64 " live_blocks_after=%" PRId64 " sum_tag_peak_bytes=%" PRId64
		" profiler_overhead_bytes=%" PRId64 "\n",
		calls,
		calls <= 8,
		spans.front().Milliseconds,
		spans.front().SelfMilliseconds,
		FrameGraph::FrameMilliseconds(),
		FrameGraph::UnmarkedMilliseconds(),
		after.TotalBytes - before.TotalBytes,
		after.TotalBlocks - before.TotalBlocks,
		afterReadings.Batch.TotalBytes - beforeReadings.Batch.TotalBytes,
		afterReadings.Batch.TotalBlocks - beforeReadings.Batch.TotalBlocks,
		afterReadings.Lookup.TotalBytes - beforeReadings.Lookup.TotalBytes,
		afterReadings.Lookup.TotalBlocks - beforeReadings.Lookup.TotalBlocks,
		afterReadings.Roots.TotalBytes - beforeReadings.Roots.TotalBytes,
		afterReadings.Roots.TotalBlocks - beforeReadings.Roots.TotalBlocks,
		afterReadings.Nodes,
		before.LiveBytes,
		after.LiveBytes,
		before.LiveBlocks,
		after.LiveBlocks,
		after.PeakBytes,
		totalsAfter.OverheadBytes
	);
}
