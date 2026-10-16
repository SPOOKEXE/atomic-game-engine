#pragma once

#include <engine/computegraph/Program.hpp>

namespace engine::computegraph {
	// Builds stable nonzero-u32 compaction using classify, hierarchical scan and scatter.
	// Input is "input"; outputs are "packed" and the device-produced scalar "count".
	// Scratch is overwritten through its full capacity on every submission, including empty input.
	// The returned declarations require independent executable reflection before execution.
	Status StockStableCompactU32(uint32_t capacity, Program &out, Diagnostic &diagnostic);
}
