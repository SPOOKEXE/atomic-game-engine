#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote the complete selected JPEG batch before output allocation.
	bool AdmitSourceJpeg(NodeContext &context, uint64_t &batchWork);
}
