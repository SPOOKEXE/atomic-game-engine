#pragma once
#include <string_view>

namespace engine::imagegraph::detail {
	// Only the source Font getter transports a path string or an owned runtime font.
	inline bool SourceFontInput(std::string_view type, std::string_view port) {
		return (type == "pc.text" && (port == "font" || port == "fallback_font")) ||
			   (type == "pc.font_data" && port == "font");
	}
}
