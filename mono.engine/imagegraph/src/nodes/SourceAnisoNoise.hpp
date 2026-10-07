#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	struct NodeContext;
	bool AdmitSourceAnisoNoise(NodeContext &context, uint64_t &batchWork);
	bool AnisotropicNoise(NodeContext &context);
}
