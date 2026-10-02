#pragma once

// Applies source processor array routing before invoking one scalar/image executor.

#include "NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	// Observes each successful selected row before publication; refusal aborts the complete batch.
	using ProcessorObserver = bool (*)(NodeContext &, void *);
	// Stages every output until all rows succeed. Inputs remain owned by the evaluator.
	bool RunProcessorBatch(
		NodeContext &context, Executor executor, ProcessorObserver observer = nullptr, void *state = nullptr
	);
}
