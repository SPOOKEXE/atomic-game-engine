#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug admit all selected Polar rows before the first output allocation.
	bool AdmitSourcePolar(NodeContext &context, uint64_t &batchWork);
}
