#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug admit every selected row before the first output allocation.
	bool AdmitSourcePixelSort(NodeContext &context, uint64_t &batchWork);
}
