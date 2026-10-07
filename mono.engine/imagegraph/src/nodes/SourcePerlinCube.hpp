#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug admit both Cube Perlin targets across all selected rows before draw.
	bool AdmitSourcePerlinCube(NodeContext &context, uint64_t &batchWork);
}
