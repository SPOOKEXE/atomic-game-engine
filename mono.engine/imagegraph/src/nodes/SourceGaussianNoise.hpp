#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote all Gaussian rows before outputs or observer calls.
	bool AdmitSourceGaussianNoise(NodeContext &context, uint64_t &batchWork);
}
