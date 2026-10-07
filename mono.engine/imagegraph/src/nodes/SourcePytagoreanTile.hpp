#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all selected PytagoreanTile rows before drawing.
	bool AdmitSourcePytagoreanTile(NodeContext &context, uint64_t &batchWork);
}
