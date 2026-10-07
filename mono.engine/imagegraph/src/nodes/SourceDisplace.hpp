#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug admit the whole selected Displace batch before output allocation.
	bool AdmitSourceDisplace(NodeContext &context, uint64_t &batchWork);
}
