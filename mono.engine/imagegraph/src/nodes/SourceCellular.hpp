#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote selected cellular rows before output allocation.
	bool AdmitSourceCellular(NodeContext &context, uint64_t &batchWork);
}
