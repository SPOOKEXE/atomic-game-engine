#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all selected Strand Noise rows before drawing.
	bool AdmitSourceStrandNoise(NodeContext &context, uint64_t &batchWork);
}
