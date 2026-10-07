#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug admit both cube noise targets across all selected rows before draw.
	bool AdmitSourceNoiseCube(NodeContext &context, uint64_t &batchWork);
}
