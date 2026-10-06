#pragma once

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>

namespace engine::imagegraph::detail {
	struct SourceInputSelection {
		size_t NodeIndex = std::numeric_limits<size_t>::max();
		std::span<const std::string_view> Ports;
	};

	inline bool
	ReadsSourceInput(const SourceInputSelection &selection, size_t consumer, std::string_view port) {
		return consumer != selection.NodeIndex ||
			   std::find(selection.Ports.begin(), selection.Ports.end(), port) != selection.Ports.end();
	}

	inline bool Active(const SourceInputSelection &selection) {
		return selection.NodeIndex != std::numeric_limits<size_t>::max();
	}
}
