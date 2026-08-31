#include <engine/render/DebugText.hpp>

#include <algorithm>
#include <array>
#include <client/SettingsMenu.hpp>
#include <string>
#include <string_view>

namespace client {

	namespace {
		std::string ToggleRow(std::string_view name, bool enabled) {
			return std::string(name) + ": " + (enabled ? "ON" : "OFF");
		}
	}

	void SettingsMenu::Toggle() {
		Open = !Open;
	}

	void SettingsMenu::Move(int direction) {
		if (!Open || direction == 0) {
			return;
		}
		if (direction < 0) {
			Selection = (Selection + ROWS - 1) % ROWS;
		} else {
			Selection = (Selection + 1) % ROWS;
		}
	}

	SettingsMenuResult SettingsMenu::Activate(Options &settings) {
		if (!Open) {
			return SettingsMenuResult::None;
		}

		switch (Selection) {
		case 0:
			settings.EnableEditableMeshes = !settings.EnableEditableMeshes;
			return SettingsMenuResult::Changed;
		case 1:
			settings.EnableEditableImages = !settings.EnableEditableImages;
			return SettingsMenuResult::Changed;
		case 2:
			settings.EnableParticles = !settings.EnableParticles;
			return SettingsMenuResult::Changed;
		case 3:
			settings.EnablePostProcessing = !settings.EnablePostProcessing;
			return SettingsMenuResult::Changed;
		case 4:
			Open = false;
			return SettingsMenuResult::Closed;
		case 5:
			return SettingsMenuResult::Quit;
		default:
			break;
		}
		return SettingsMenuResult::None;
	}

	void
	DrawSettingsMenu(engine::render::OverlayImage &image, const Options &settings, const SettingsMenu &menu) {
		if (!menu.IsOpen() || image.IsEmpty()) {
			return;
		}

		const int scale = image.GetWidth() >= 2400 ? 3 : 2;
		const int line = engine::render::DebugText::LineHeight(scale);
		const int panelWidth = std::min(image.GetWidth() - 24 * scale, 260 * scale);
		const int panelHeight = 10 * line;
		const int left = (image.GetWidth() - panelWidth) / 2;
		const int top = (image.GetHeight() - panelHeight) / 2;

		image.Fill(0, 0, image.GetWidth(), image.GetHeight(), 0, 0, 0, 150);
		image.Blend(left, top, panelWidth, panelHeight, 18, 24, 34, 245);
		image.Blend(left, top, panelWidth, 2 * scale, 80, 170, 255, 255);

		const std::array<std::string, SettingsMenu::ROWS> rows{
			ToggleRow("EDITABLE MESH UPDATES", settings.EnableEditableMeshes),
			ToggleRow("EDITABLE IMAGE UPDATES", settings.EnableEditableImages),
			ToggleRow("PARTICLES", settings.EnableParticles),
			ToggleRow("POST PROCESSING", settings.EnablePostProcessing),
			"RESUME",
			"QUIT",
		};

		const int textLeft = left + 12 * scale;
		int y = top + line;
		engine::render::DebugText::Draw(image, textLeft, y, "SETTINGS", 130, 205, 255, scale);
		y += line * 2;
		for (size_t index = 0; index < rows.size(); index++) {
			const bool active = index == menu.Selected();
			if (active) {
				image.Blend(left + 6 * scale, y - scale, panelWidth - 12 * scale, line, 50, 90, 130, 220);
			}
			engine::render::DebugText::Draw(
				image,
				textLeft,
				y,
				(active ? "> " : "  ") + rows[index],
				active ? 255 : 205,
				active ? 255 : 215,
				active ? 255 : 225,
				scale
			);
			y += line;
		}

		engine::render::DebugText::Draw(
			image, textLeft, top + panelHeight - line, "UP/DOWN  ENTER  ESC", 130, 145, 165, scale
		);
	}
}
