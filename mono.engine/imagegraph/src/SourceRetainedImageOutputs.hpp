#pragma once
#include "NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	// Whole-output source skip behavior runs before processor row selection.
	bool SourceRetainedProcessorInactive(NodeContext &, bool &handled);
	// Capture the fully folded output, including its source array shape.
	bool CaptureSourceRetainedProcessorOutputs(NodeContext &);
	// Manual nodes store their complete metadata record in the same caller-owned ledger.
	const Value *SourceRetainedField(NodeContext &, std::string_view port);
	bool StoreSourceRetainedMetadata(
		NodeContext &, std::span<const std::pair<std::string_view, const Value *>> fields
	);
}
