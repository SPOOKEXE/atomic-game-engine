#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote each selected row before any Bend output surface is allocated.
	bool AdmitSourceBend(NodeContext &context, uint64_t &batchWork);
}
