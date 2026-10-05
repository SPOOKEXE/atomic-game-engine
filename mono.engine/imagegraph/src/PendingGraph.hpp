#pragma once

#include "EvaluationAllocator.hpp"
#include "SourceFrameCacheInputs.hpp"

#include <engine/imagegraph/CacheGroupReplay.hpp>

namespace engine::imagegraph::detail {
	// Rebuild cuts from structural edges so a later wake can restore unread dependencies.
	struct PendingGraph {
		using Sources = EvaluationVector<size_t>;
		EvaluationBudget &Budget;
		EvaluationVector<Sources> Upstream;
		EvaluationVector<uint8_t> Needed;
		EvaluationVector<size_t> RemainingConsumers;
		explicit PendingGraph(EvaluationBudget &budget);
		// A completed producer retains its output without admitting its earlier inputs again.
		// Failure preserves the previous graph; prior and candidate tables coexist under Budget.
		Status Rebuild(
			const Document &document,
			const Plan &plan,
			std::span<const SourceFrameCacheInputReads> reads,
			std::span<const CacheGroupReplayNode *const> frozen,
			std::span<const size_t> roots,
			std::span<const PcxNamedDependency> dynamicRoutes,
			std::span<const uint8_t> completed,
			bool cutInputs,
			uint64_t &work,
			Diagnostic &diagnostic
		);
	};
}
