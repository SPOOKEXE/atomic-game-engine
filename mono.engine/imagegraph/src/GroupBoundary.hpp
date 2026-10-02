#pragma once
#include "NodeExecutors.hpp"
namespace engine::imagegraph::detail {
	bool ResolveGroupSocketDomain(int64_t type, int64_t subtype, int64_t size, SourceSocketDomain &out);
	bool ExecuteGroupBoundary(NodeContext &context);
}
