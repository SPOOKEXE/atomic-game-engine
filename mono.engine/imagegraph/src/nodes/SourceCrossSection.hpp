#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	bool AdmitSourceCrossSection(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes);
	bool DrawSourceCrossSection(NodeContext &context);
}
