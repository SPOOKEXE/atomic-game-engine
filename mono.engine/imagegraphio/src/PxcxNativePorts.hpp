#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace engine::imagegraphio::detail {
	// legacy pins keep their native names. saving links must reuse the import map.
	inline std::string_view LegacyNativeOutputPort(std::string_view type, uint32_t index) {
		if (type == "Node_Number") return index == 0 ? "number" : std::string_view{};
		if (type == "Node_Math") return index == 0 ? "result" : std::string_view{};
		if (type == "Node_Shape") {
			constexpr std::array<std::string_view, 4> outputs = {"colored", "mask", "height", "uv"};
			return index < outputs.size() ? outputs[index] : std::string_view{};
		}
		return index == 0 ? "image" : std::string_view{};
	}

}
