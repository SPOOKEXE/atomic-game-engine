#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all selected Extra Perlin rows before drawing.
	bool AdmitSourcePerlinExtra(NodeContext &context, uint64_t &batchWork);
}
