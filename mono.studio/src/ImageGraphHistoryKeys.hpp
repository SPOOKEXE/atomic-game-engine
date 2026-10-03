#pragma once

#include <imgui.h>
#include <utility>

namespace studio::detail {
	enum class ImageGraphHistoryKey : unsigned char { None, Undo, Redo };

	// A history shortcut belongs to the composer and its panels, but never to an
	// editor field or popup.
	inline ImageGraphHistoryKey ImageGraphHistoryKeyPressed() {
		const ImGuiIO &io = ImGui::GetIO();
		if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::IsAnyItemActive() ||
			io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || io.KeyAlt)
			return ImageGraphHistoryKey::None;

		// Dear ImGui may map the macOS Command key to either modifier.
		if (!(io.KeyCtrl || io.KeySuper)) return ImageGraphHistoryKey::None;
		if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
			return io.KeyShift ? ImageGraphHistoryKey::Redo : ImageGraphHistoryKey::Undo;
		if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y, false)) return ImageGraphHistoryKey::Redo;
		return ImageGraphHistoryKey::None;
	}

	template <class Apply> void ApplyImageGraphHistoryKey(Apply &&apply) {
		const ImageGraphHistoryKey key = ImageGraphHistoryKeyPressed();
		if (key == ImageGraphHistoryKey::None) return;
		std::forward<Apply>(apply)(key == ImageGraphHistoryKey::Redo);
	}
} // namespace studio::detail
