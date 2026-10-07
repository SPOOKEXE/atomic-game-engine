#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all selected noise rows before any output allocation.
	bool AdmitSourceNoise(NodeContext &context, uint64_t &batchWork);
}
