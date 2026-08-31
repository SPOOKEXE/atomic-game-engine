#pragma once

// The shipped client's keyboard-driven ESC settings menu.
// @tier L13 · client

#include <engine/core/Name.hpp>
#include <engine/gui/SettingsMenu.hpp>

#include <client/Options.hpp>
#include <cstddef>
#include <span>

namespace engine::render {
	class OverlayImage;
}

namespace client {

	enum class SettingsMenuResult {
		None,
		Changed,
		Action,
		Closed,
		Quit,
	};

	struct SettingsMenuActivation {
		SettingsMenuResult Result = SettingsMenuResult::None;
		engine::core::Name Action;
	};

	class SettingsMenu {
	  public:
		static constexpr size_t BUILTIN_TOGGLES = 4;
		static constexpr size_t FIXED_ROWS = 6;

		void Toggle();
		void Move(int direction, size_t actionCount = 0);
		SettingsMenuActivation
		Activate(Options &settings, std::span<const engine::gui::SettingsMenuAction> actions = {});

		bool IsOpen() const {
			return Open;
		}

		size_t Selected(size_t actionCount = 0) const {
			return Selection % (FIXED_ROWS + actionCount);
		}

	  private:
		bool Open = false;
		size_t Selection = 0;
	};

	// Draws the menu into the renderer-independent host overlay.
	void DrawSettingsMenu(
		engine::render::OverlayImage &image,
		const Options &settings,
		const SettingsMenu &menu,
		std::span<const engine::gui::SettingsMenuAction> actions = {}
	);
}
