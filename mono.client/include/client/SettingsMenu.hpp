#pragma once

// The shipped client's keyboard-driven ESC settings menu.
// @tier L13 · client

#include <client/Options.hpp>
#include <cstddef>

namespace engine::render {
	class OverlayImage;
}

namespace client {

	enum class SettingsMenuResult {
		None,
		Changed,
		Closed,
		Quit,
	};

	class SettingsMenu {
	  public:
		static constexpr size_t ROWS = 6;

		void Toggle();
		void Move(int direction);
		SettingsMenuResult Activate(Options &settings);

		bool IsOpen() const {
			return Open;
		}

		size_t Selected() const {
			return Selection;
		}

	  private:
		bool Open = false;
		size_t Selection = 0;
	};

	// Draws the menu into the renderer-independent host overlay.
	void
	DrawSettingsMenu(engine::render::OverlayImage &image, const Options &settings, const SettingsMenu &menu);
}
