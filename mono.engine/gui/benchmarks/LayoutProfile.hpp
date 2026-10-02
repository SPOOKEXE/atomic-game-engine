#pragma once

#include "../src/LayoutScratch.hpp"
#include "../tests/fixtures/LayoutOracle.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace layout_bench {
	constexpr std::string_view OWNER = "gui layout benchmark";
	constexpr std::string_view LAYOUT = "gui layout";
	constexpr std::string_view CHILD_SCAN = "gui child scan";
	constexpr std::string_view ROOTS = "gui collector root snapshot";
	constexpr std::string_view COLLECTORS = "gui collector handle snapshot";
	struct HeapReading {
		engine::core::HeapNodeView Owner;
		engine::core::HeapNodeView LayoutExclusive;
		engine::core::HeapNodeView LayoutInclusive;
		engine::core::HeapNodeView ChildScan;
		engine::core::HeapNodeView Roots;
		engine::core::HeapNodeView Collectors;
	};
	inline void Add(engine::core::HeapNodeView &total, const engine::core::HeapNodeView &node) {
		total.TotalBytes += node.TotalBytes;
		total.TotalBlocks += node.TotalBlocks;
		total.LiveBytes += node.LiveBytes;
		total.LiveBlocks += node.LiveBlocks;
		total.PeakBytes += node.PeakBytes;
	}
	inline HeapReading ReadHeap() {
		using engine::core::HeapProfile;
		const uint32_t count = HeapProfile::NodeCount();
		if (count > 4096) throw std::runtime_error("GUI layout heap tree exceeds diagnostic bound");
		HeapReading result;
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Name == OWNER) Add(result.Owner, node);
			if (node.Name == LAYOUT) Add(result.LayoutExclusive, node);
			if (node.Name == CHILD_SCAN) Add(result.ChildScan, node);
			if (node.Name == ROOTS) Add(result.Roots, node);
			if (node.Name == COLLECTORS) Add(result.Collectors, node);
			uint32_t ancestor = index;
			for (uint32_t depth = 0; ancestor != 0; ++depth) {
				if (depth >= count || ancestor >= count)
					throw std::runtime_error("GUI layout heap parent chain malformed");
				const auto parent = HeapProfile::Node(ancestor);
				if (parent.Name == LAYOUT) {
					Add(result.LayoutInclusive, node);
					break;
				}
				ancestor = parent.Parent;
			}
		}
		return result;
	}
	inline bool Enabled() {
		static const bool enabled = [] {
			const char *value = std::getenv("ATOMIC_GUI_LAYOUT_PROFILE");
			return value != nullptr && std::string_view(value) == "1";
		}();
		return enabled;
	}
	struct RestoreProfile {
		bool Previous = engine::core::FrameGraph::IsEnabled();
		~RestoreProfile() {
			engine::core::FrameGraph::SetEnabled(Previous);
		}
	};
	template <class Body, class After> void Capture(size_t row, Body &&body, After &&afterBody) {
		static size_t calls = 0;
		if (!Enabled()) {
			body();
			if (layout_fixture::CanonicalEnabled()) {
				if (row >= 6 || calls == 128) throw std::runtime_error("GUI canonical call bound exceeded");
				afterBody(++calls);
			}
			return;
		}
		using engine::core::FrameGraph;
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn() || row >= 6 || calls == 128)
			throw std::runtime_error("GUI layout capture requires heap hooks and at most 128 calls per row");
		const RestoreProfile restore;
		FrameGraph::SetEnabled(true);
		const auto scratchBefore = engine::gui::detail::ReadChildArenaScratch();
		const auto before = ReadHeap();
		const auto totalBefore = HeapProfile::Totals();
		FrameGraph::BeginFrame();
		try {
			ENGINE_PROFILE("gui layout benchmark");
			body();
		} catch (...) {
			// Finish an exceptional frame before restoring the collector setting.
			FrameGraph::EndFrame();
			throw;
		}
		FrameGraph::EndFrame();
		const auto &spans = FrameGraph::Spans();
		if (FrameGraph::Dropped() != 0 || spans.size() < 2 || spans.size() > 3 ||
			spans.front().Name != OWNER || spans.front().Parent != FrameGraph::NO_PARENT ||
			spans.front().Depth != 0 || spans.front().Reported)
			throw std::runtime_error("GUI layout capture incomplete owner hierarchy or dropped spans");
		float layoutInclusive = 0, layoutSelf = 0;
		// The first warmup may also contain TreeOf's original setup layout.
		// Every completed frame is consumed now, not inferred from retained history.
		for (size_t index = 1; index < spans.size(); ++index) {
			const auto &span = spans[index];
			if (span.Name != LAYOUT || span.Parent != 0 || span.Depth != 1 || span.Reported ||
				span.Category != engine::core::ProfileCategory::ECS)
				throw std::runtime_error("GUI layout capture unexpected nested hierarchy");
			layoutInclusive += span.Milliseconds;
			layoutSelf += span.SelfMilliseconds;
		}
		const auto scratchAfter = engine::gui::detail::ReadChildArenaScratch();
		if (scratchBefore.LogicalHandles != 0 || scratchAfter.LogicalHandles != 0 ||
			scratchAfter.CapacityHandles < scratchBefore.CapacityHandles ||
			scratchBefore.PayloadBytes != scratchBefore.CapacityHandles * sizeof(engine::ecs::Entity) ||
			scratchAfter.PayloadBytes != scratchAfter.CapacityHandles * sizeof(engine::ecs::Entity))
			throw std::runtime_error("GUI arena logical lifetime or capacity ledger invalid");
		static size_t steadyCapacity = 0;
		if (calls != 0 && scratchAfter.CapacityHandles != steadyCapacity)
			throw std::runtime_error("GUI repeated fixture arena did not plateau");
		steadyCapacity = scratchAfter.CapacityHandles;
		const auto after = ReadHeap();
		const auto totalAfter = HeapProfile::Totals();
		if (totalAfter.DroppedScopes != totalBefore.DroppedScopes)
			throw std::runtime_error("GUI layout capture dropped heap scopes");
		const auto bytes = [](const auto &a, const auto &b) { return a.TotalBytes - b.TotalBytes; };
		const auto blocks = [](const auto &a, const auto &b) { return a.TotalBlocks - b.TotalBlocks; };
		const uint64_t childScanBytes = bytes(after.ChildScan, before.ChildScan);
		const uint64_t childScanBlocks = blocks(after.ChildScan, before.ChildScan);
		const uint64_t rootBytes = bytes(after.Roots, before.Roots);
		const uint64_t rootBlocks = blocks(after.Roots, before.Roots);
		const uint64_t collectorBytes = bytes(after.Collectors, before.Collectors);
		const uint64_t collectorBlocks = blocks(after.Collectors, before.Collectors);
		const uint64_t layoutBytes = bytes(after.LayoutInclusive, before.LayoutInclusive);
		const uint64_t layoutBlocks = blocks(after.LayoutInclusive, before.LayoutInclusive);
		const uint64_t exclusiveBytes = bytes(after.LayoutExclusive, before.LayoutExclusive);
		const uint64_t exclusiveBlocks = blocks(after.LayoutExclusive, before.LayoutExclusive);
		const uint64_t ownerBytes = bytes(after.Owner, before.Owner);
		const uint64_t ownerBlocks = blocks(after.Owner, before.Owner);
		const uint64_t processBytes = bytes(totalAfter, totalBefore);
		const uint64_t processBlocks = blocks(totalAfter, totalBefore);
		if (rootBytes + collectorBytes + childScanBytes + exclusiveBytes > layoutBytes ||
			rootBlocks + collectorBlocks + childScanBlocks + exclusiveBlocks > layoutBlocks ||
			layoutBytes + ownerBytes > processBytes || layoutBlocks + ownerBlocks > processBlocks)
			throw std::runtime_error("GUI layout exclusive heap counters fail inclusive reconciliation");
		const size_t call = ++calls;
		std::printf(
			"# gui-layout-profile row=%zu call=%zu warmup=%d frames=1 spans=%zu layout_calls=%zu "
			"frame_drops=0 heap_drop_delta=0 owner_inclusive_ms=%.6f owner_self_ms=%.6f "
			"layout_inclusive_ms=%.6f layout_self_ms=%.6f frame_ms=%.6f unmarked_ms=%.6f "
			"idle_ms=0 reported_spans=0 heap_coverage=cxx_new_delete "
			"child_scan_bytes=%" PRIu64 " child_scan_blocks=%" PRIu64 " root_bytes=%" PRIu64
			" root_blocks=%" PRIu64 " collector_bytes=%" PRIu64 " collector_blocks=%" PRIu64
			" layout_inclusive_bytes=%" PRIu64 " layout_inclusive_blocks=%" PRIu64
			" layout_exclusive_bytes=%" PRIu64 " layout_exclusive_blocks=%" PRIu64
			" other_layout_bytes=%" PRIu64 " other_layout_blocks=%" PRIu64 " owner_exclusive_bytes=%" PRIu64
			" owner_exclusive_blocks=%" PRIu64 " process_bytes=%" PRIu64 " process_blocks=%" PRIu64
			" other_process_bytes=%" PRIu64 " other_process_blocks=%" PRIu64
			" child_scan_live_before=%" PRId64 " child_scan_live_after=%" PRId64
			" child_scan_live_blocks_before=%" PRId64 " child_scan_live_blocks_after=%" PRId64
			" child_scan_sum_tag_peak_bytes=%" PRId64 " root_live_before=%" PRId64 " root_live_after=%" PRId64
			" root_live_blocks_before=%" PRId64 " root_live_blocks_after=%" PRId64
			" collector_live_before=%" PRId64 " collector_live_after=%" PRId64
			" collector_live_blocks_before=%" PRId64 " collector_live_blocks_after=%" PRId64
			" root_sum_tag_peak_bytes=%" PRId64 " collector_sum_tag_peak_bytes=%" PRId64
			" process_live_before=%" PRId64 " process_live_after=%" PRId64
			" process_live_blocks_before=%" PRId64 " process_live_blocks_after=%" PRId64
			" profiler_overhead_bytes=%" PRId64 "\n",
			row,
			call,
			call <= 8,
			spans.size(),
			spans.size() - 1,
			spans.front().Milliseconds,
			spans.front().SelfMilliseconds,
			layoutInclusive,
			layoutSelf,
			FrameGraph::FrameMilliseconds(),
			FrameGraph::UnmarkedMilliseconds(),
			childScanBytes,
			childScanBlocks,
			rootBytes,
			rootBlocks,
			collectorBytes,
			collectorBlocks,
			layoutBytes,
			layoutBlocks,
			exclusiveBytes,
			exclusiveBlocks,
			layoutBytes - rootBytes - collectorBytes - childScanBytes - exclusiveBytes,
			layoutBlocks - rootBlocks - collectorBlocks - childScanBlocks - exclusiveBlocks,
			ownerBytes,
			ownerBlocks,
			processBytes,
			processBlocks,
			processBytes - layoutBytes - ownerBytes,
			processBlocks - layoutBlocks - ownerBlocks,
			before.ChildScan.LiveBytes,
			after.ChildScan.LiveBytes,
			before.ChildScan.LiveBlocks,
			after.ChildScan.LiveBlocks,
			after.ChildScan.PeakBytes,
			before.Roots.LiveBytes,
			after.Roots.LiveBytes,
			before.Roots.LiveBlocks,
			after.Roots.LiveBlocks,
			before.Collectors.LiveBytes,
			after.Collectors.LiveBytes,
			before.Collectors.LiveBlocks,
			after.Collectors.LiveBlocks,
			after.Roots.PeakBytes,
			after.Collectors.PeakBytes,
			totalBefore.LiveBytes,
			totalAfter.LiveBytes,
			totalBefore.LiveBlocks,
			totalAfter.LiveBlocks,
			totalAfter.OverheadBytes
		);
		std::printf(
			"# gui-layout-arena row=%zu call=%zu logical_before=%zu logical_after=%zu capacity_before=%zu "
			"capacity_after=%zu payload_bytes_before=%zu payload_bytes_after=%zu "
			"retention=thread_local_high_water fixed_ceiling=0\n",
			row,
			call,
			scratchBefore.LogicalHandles,
			scratchAfter.LogicalHandles,
			scratchBefore.CapacityHandles,
			scratchAfter.CapacityHandles,
			scratchBefore.PayloadBytes,
			scratchAfter.PayloadBytes
		);
		// Correctness export is outside the measured owner; enclosing BENCH
		// times are unusable in this opt-in mode and must not enter A/B timing.
		if (layout_fixture::CanonicalEnabled()) afterBody(call);
	}
} // namespace layout_bench
