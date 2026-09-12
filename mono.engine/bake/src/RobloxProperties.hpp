#pragma once

#include <string_view>

namespace engine::bake {

	// Roblox writes these private storage names in binary models and occasionally
	// in XML exports. The neutral tree always exposes the public API spelling.
	inline std::string_view PublicRobloxPropertyName(std::string_view storedName) {
		if (storedName == "size") {
			return "Size";
		}
		return storedName == "Color3uint8" ? "Color" : storedName;
	}
}
