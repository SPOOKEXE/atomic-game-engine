#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all selected Weave rows before drawing.
	bool AdmitSourceWeave(NodeContext &context, uint64_t &batchWork);
}
