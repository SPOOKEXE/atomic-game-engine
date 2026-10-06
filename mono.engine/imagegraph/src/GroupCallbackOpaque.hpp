#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	// grug keep unknown dependencies visible in callback scratch; this node can never run.
	inline constexpr std::string_view GroupCallbackOpaqueType = "internal.group_opaque";
	inline bool IsGroupCallbackOpaque(const Node &node) {
		return node.Type == GroupCallbackOpaqueType;
	}
}
