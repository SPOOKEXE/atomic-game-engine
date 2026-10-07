#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote every selected caustic row before drawing first output.
	bool AdmitSourceCaustic(NodeContext &context, uint64_t &batchWork);
}
