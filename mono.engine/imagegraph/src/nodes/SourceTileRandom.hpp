#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	bool AdmitSourceTileRandom(NodeContext &context, uint64_t &batchWork);
	bool DrawSourceTileRandom(NodeContext &context);
}
