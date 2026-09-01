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

	// What activating the selected settings-menu row asked the client to do.
	enum class SettingsMenuResult {
		None,
		Changed,
		Action,
		Closed,
		Quit,
	};

	// The result and optional script action produced by one activation.
	struct SettingsMenuActivation {
		// The built-in operation selected by the row.
		SettingsMenuResult Result = SettingsMenuResult::None;

		// The script action identifier when `Result` is `Action`.
		engine::core::Name Action;
	};

	// Keyboard navigation state for the shipped client's ESC settings menu.
	class SettingsMenu {
	  public:
		// The number of built-in presentation capability toggles.
		static constexpr size_t BUILTIN_TOGGLES = 4;

		// The number of fixed rows, including close and quit.
		static constexpr size_t FIXED_ROWS = 6;

		// Opens a closed menu or closes an open one.
		void Toggle();

		// Moves the selected row with wraparound.
		void Move(int direction, size_t actionCount = 0);

		// Activates the selected built-in or script-authored row.
		SettingsMenuActivation
		Activate(Options &settings, std::span<const engine::gui::SettingsMenuAction> actions = {});

		// Reports whether the menu currently owns keyboard navigation.
		bool IsOpen() const {
			return Open;
		}

		// Returns the selected row after accounting for script-authored actions.
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
