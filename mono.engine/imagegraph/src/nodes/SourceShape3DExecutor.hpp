#pragma once
#include <cstdint>
namespace engine::imagegraph::detail {
	class NodeContext;
	bool AdmitSourceShape3D(NodeContext &context, uint64_t &batchWork);
}
