#pragma once

#include <engine/core/Name.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>

#include <algorithm>
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

	struct FramePlanDiagnosticCache {
		// The owner must change this whenever the installed graph or schedule changes.
		uint64_t PipelineRevision = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint64_t> Worlds;
		FramePlanDiagnosticTotals Totals;
		bool Ready = false;
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
		if (cache.Ready && cache.PipelineRevision == pipelineRevision && cache.Width == width &&
			cache.Height == height && cache.Worlds.size() == worlds.size() &&
			cache.Totals.CommandBuffers == commandBuffers &&
			std::equal(cache.Worlds.begin(), cache.Worlds.end(), worlds.begin())) {
			offender = core::Name{};
			return {.Status = graph::ExecutionPlanStatus::Ok, .Totals = cache.Totals, .CacheHit = true};
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
			cache.Worlds.swap(cachedWorlds);
			cache.PipelineRevision = pipelineRevision;
			cache.Width = width;
			cache.Height = height;
			cache.Totals = totals;
			cache.Ready = true;
		} catch (const std::bad_alloc &) {
			// A cache-key allocation failure must not turn a successful plan into a refusal.
		}

		return {.Status = graph::ExecutionPlanStatus::Ok, .Totals = totals};
	}
}
