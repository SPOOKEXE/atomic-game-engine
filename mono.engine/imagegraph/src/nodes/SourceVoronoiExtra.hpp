#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote every selected Extra Voronoi row before drawing.
	bool AdmitSourceVoronoiExtra(NodeContext &context, uint64_t &batchWork);
}
