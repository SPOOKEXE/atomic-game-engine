#pragma once

// grug ordinary Group inherits base Collection sockets and rendering, but owns its instance callback.

#include <string_view>

namespace engine::imagegraphio::detail {
	inline bool IsOrdinarySourceGroup(std::string_view type) {
		return type == "Node_Group" || type == "Node_Collection";
	}
}
