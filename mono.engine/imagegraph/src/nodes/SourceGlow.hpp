#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote every selected row before any Glow output image is allocated.
	bool AdmitSourceGlow(NodeContext &context, uint64_t &batchWork);
}
