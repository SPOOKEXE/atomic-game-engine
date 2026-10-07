#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all Scratch rows before outputs or observer calls.
	bool AdmitSourceScratchNoise(NodeContext &context, uint64_t &batchWork);
}
