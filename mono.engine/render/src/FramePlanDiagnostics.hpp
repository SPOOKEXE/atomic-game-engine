#pragma once

#include <engine/core/Name.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

namespace engine::render {
	struct FramePlanDiagnosticTotals {
		uint64_t ReadBytes = 0;
		uint64_t WriteBytes = 0;
		uint64_t QueueTransferBytes = 0;
		uint32_t ConcurrentWaves = 0;
		uint32_t CommandBuffers = 0;
	};

	// Recent plan totals for one installed pipeline.
	//
	// **Several entries, because one pipeline serves several view sets.** An
	// editor draws each viewport's world in its own batch, so a single entry
	// was replaced by every viewport in turn and the plan was worked out again
	// for each of them on every frame.
	struct FramePlanDiagnosticCache {
		struct Entry {
			// The owner must change this whenever the installed graph or schedule changes.
			uint64_t PipelineRevision = 0;
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::vector<uint64_t> Worlds;
			FramePlanDiagnosticTotals Totals;
			bool Ready = false;
		};
		std::array<Entry, 8> Entries;
		// The entry the next miss replaces, oldest first.
		size_t Next = 0;
	};

	struct FramePlanDiagnosticResult {
		graph::ExecutionPlanStatus Status = graph::ExecutionPlanStatus::Ok;
		FramePlanDiagnosticTotals Totals;
		bool CacheHit = false;
	};

	inline FramePlanDiagnosticResult ResolveFramePlanDiagnostics(
		const graph::RenderGraph &graph,
		const graph::ExecutionSchedule &schedule,
		uint64_t pipelineRevision,
		std::span<const uint64_t> worlds,
		uint32_t width,
		uint32_t height,
		uint32_t commandBuffers,
		FramePlanDiagnosticCache &cache,
		core::Name &offender
	) {
		for (const FramePlanDiagnosticCache::Entry &entry : cache.Entries) {
			if (entry.Ready && entry.PipelineRevision == pipelineRevision && entry.Width == width &&
				entry.Height == height && entry.Worlds.size() == worlds.size() &&
				entry.Totals.CommandBuffers == commandBuffers &&
				std::equal(entry.Worlds.begin(), entry.Worlds.end(), worlds.begin())) {
				offender = core::Name{};
				return {.Status = graph::ExecutionPlanStatus::Ok, .Totals = entry.Totals, .CacheHit = true};
			}
		}

		graph::FrameExecutionPlan plan;
		const graph::ExecutionPlanStatus status =
			graph::PlanFrame(graph, schedule, worlds, width, height, plan, offender);
		if (status != graph::ExecutionPlanStatus::Ok) {
			return {.Status = status, .Totals = {}};
		}

		FramePlanDiagnosticTotals totals{
			.ReadBytes = plan.ReadBytes,
			.WriteBytes = plan.WriteBytes,
			.QueueTransferBytes = plan.QueueTransferBytes,
			.ConcurrentWaves = static_cast<uint32_t>(std::count_if(
				plan.Waves.begin(),
				plan.Waves.end(),
				[](const graph::PlannedWave &wave) { return wave.ConcurrentQueues; }
			)),
			.CommandBuffers = commandBuffers,
		};

		try {
			std::vector<uint64_t> cachedWorlds(worlds.begin(), worlds.end());
			FramePlanDiagnosticCache::Entry &entry = cache.Entries[cache.Next];
			cache.Next = (cache.Next + 1) % cache.Entries.size();
			entry.Worlds.swap(cachedWorlds);
			entry.PipelineRevision = pipelineRevision;
			entry.Width = width;
			entry.Height = height;
			entry.Totals = totals;
			entry.Ready = true;
		} catch (const std::bad_alloc &) {}

		return {.Status = graph::ExecutionPlanStatus::Ok, .Totals = totals};
	}
}
