#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug admit selected rows before any Pixel Math output is allocated.
	bool AdmitSourcePixelMath(NodeContext &context, uint64_t &batchWork);
}
