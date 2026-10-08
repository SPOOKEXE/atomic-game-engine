#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	bool AdmitSourcePathWave(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes);
	bool ExecuteSourcePathWave(NodeContext &context);
}
