#pragma once

#include "../NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	bool Camera(NodeContext &context);
	bool AdmitSourceCamera(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes);
}
