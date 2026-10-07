#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote complete selected Perlin batch before output allocation.
	bool AdmitSourcePerlin(NodeContext &context, uint64_t &batchWork);
}
