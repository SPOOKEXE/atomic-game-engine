#pragma once

#include <imgui.h>
#include <string>

namespace studio {
	// ImGui queues Unicode scalar values; game GUI typing consumes UTF-8.
	inline std::string GuiInputText(const ImGuiIO &io) {
		std::string text;
		for (const unsigned int character : io.InputQueueCharacters) {
			if (character == 0 || character > 0x10FFFF || (character >= 0xD800 && character <= 0xDFFF)) {
				continue;
			}
			if (character < 0x80) {
				text.push_back(static_cast<char>(character));
			} else if (character < 0x800) {
				text.push_back(static_cast<char>(0xC0 | (character >> 6)));
				text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
			} else if (character < 0x10000) {
				text.push_back(static_cast<char>(0xE0 | (character >> 12)));
				text.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
				text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
			} else {
				text.push_back(static_cast<char>(0xF0 | (character >> 18)));
				text.push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
				text.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
				text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
			}
		}
		return text;
	}
}
