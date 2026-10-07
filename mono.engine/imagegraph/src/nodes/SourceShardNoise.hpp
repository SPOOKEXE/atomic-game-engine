#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all selected Shard Noise rows before drawing.
	bool AdmitSourceShardNoise(NodeContext &context, uint64_t &batchWork);
}
