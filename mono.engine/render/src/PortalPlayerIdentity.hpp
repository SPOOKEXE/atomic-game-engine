#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::render {
	inline bool ValidPortalPlayerIdentity(std::string_view text) {
		if (text.empty()) return true;
		if (text.size() > 20) return false;
		int64_t player = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), player);
		return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
			   std::to_string(player) == text;
	}
}
