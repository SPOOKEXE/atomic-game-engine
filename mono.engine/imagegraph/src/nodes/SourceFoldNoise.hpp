#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all Fold rows before outputs or observer calls.
	bool AdmitSourceFoldNoise(NodeContext &context, uint64_t &batchWork);
}
