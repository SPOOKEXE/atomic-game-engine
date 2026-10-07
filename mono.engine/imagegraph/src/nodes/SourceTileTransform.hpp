#pragma once
#include "../NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	bool SourceTilePreviewIndex(NodeContext &context, size_t rows, size_t &index);
	bool SourceTileReferenceDimension(NodeContext &context, Vector2 &dimension);
	bool AdmitSourceTileTransform(NodeContext &context, uint64_t &work);
}
