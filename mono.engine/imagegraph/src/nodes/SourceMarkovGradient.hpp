#pragma once

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NodeContext;
	bool ReadSourceMarkovActive(NodeContext &context, bool &active);
	bool AdmitSourceMarkovGradient(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes);
	bool DrawSourceMarkovGradient(NodeContext &context);
}
