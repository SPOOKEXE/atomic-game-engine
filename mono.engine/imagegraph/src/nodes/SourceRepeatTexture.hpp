#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	bool AdmitSourceRepeatTexture(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes);
	bool DrawSourceRepeatTexture(NodeContext &context);
}
