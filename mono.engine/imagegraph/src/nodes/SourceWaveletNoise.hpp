#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	// grug quote every selected Wavelet row before output allocation.
	bool AdmitSourceWaveletNoise(NodeContext &context, uint64_t &batchWork);
}
