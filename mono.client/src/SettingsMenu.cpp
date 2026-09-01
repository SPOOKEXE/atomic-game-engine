#include <engine/render/DebugText.hpp>

#include <algorithm>
#include <array>
#include <client/SettingsMenu.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace client {

	namespace {
		std::string ToggleRow(std::string_view name, bool enabled) {
			return std::string(name) + ": " + (enabled ? "ON" : "OFF");
		}
	}

	void SettingsMenu::Toggle() {
		Open = !Open;
	}

	void SettingsMenu::Move(int direction, size_t actionCount) {
		if (!Open || direction == 0) {
			return;
		}
		const size_t rows = FIXED_ROWS + actionCount;
		Selection %= rows;
		if (direction < 0) {
			Selection = (Selection + rows - 1) % rows;
		} else {
			Selection = (Selection + 1) % rows;
		}
	}

	SettingsMenuActivation
	SettingsMenu::Activate(Options &settings, std::span<const engine::gui::SettingsMenuAction> actions) {
		if (!Open) {
			return {};
		}
		Selection %= FIXED_ROWS + actions.size();

		switch (Selection) {
		case 0:
			settings.EnableEditableMeshes = !settings.EnableEditableMeshes;
			return {SettingsMenuResult::Changed, {}};
		case 1:
			settings.EnableEditableImages = !settings.EnableEditableImages;
			return {SettingsMenuResult::Changed, {}};
		case 2:
			settings.EnableParticles = !settings.EnableParticles;
			return {SettingsMenuResult::Changed, {}};
		case 3:
			settings.EnablePostProcessing = !settings.EnablePostProcessing;
			return {SettingsMenuResult::Changed, {}};
		default:
			break;
		}

		const size_t action = Selection - BUILTIN_TOGGLES;
		if (action < actions.size()) {
			return {SettingsMenuResult::Action, actions[action].Id};
		}
		if (action == actions.size()) {
			Open = false;
			return {SettingsMenuResult::Closed, {}};
		}
		return {SettingsMenuResult::Quit, {}};
	}

	void DrawSettingsMenu(
		engine::render::OverlayImage &image,
		const Options &settings,
		const SettingsMenu &menu,
		std::span<const engine::gui::SettingsMenuAction> actions
	) {
		if (!menu.IsOpen() || image.IsEmpty()) {
			return;
		}

		const int scale = image.GetWidth() >= 2400 ? 3 : 2;
		const int line = engine::render::DebugText::LineHeight(scale);
		const int panelWidth = std::min(image.GetWidth() - 24 * scale, 260 * scale);
		const size_t rowCount = SettingsMenu::FIXED_ROWS + actions.size();
		const int panelHeight = static_cast<int>(rowCount + 4) * line;
		const int left = (image.GetWidth() - panelWidth) / 2;
		const int top = (image.GetHeight() - panelHeight) / 2;

		image.Fill(0, 0, image.GetWidth(), image.GetHeight(), 0, 0, 0, 150);
		image.Blend(left, top, panelWidth, panelHeight, 18, 24, 34, 245);
		image.Blend(left, top, panelWidth, 2 * scale, 80, 170, 255, 255);

		const std::array<std::string, SettingsMenu::BUILTIN_TOGGLES> toggles{
			ToggleRow("EDITABLE MESH UPDATES", settings.EnableEditableMeshes),
			ToggleRow("EDITABLE IMAGE UPDATES", settings.EnableEditableImages),
			ToggleRow("PARTICLES", settings.EnableParticles),
			ToggleRow("POST PROCESSING", settings.EnablePostProcessing),
		};
		std::vector<std::string> rows(toggles.begin(), toggles.end());
		rows.reserve(rowCount);
		for (const engine::gui::SettingsMenuAction &action : actions) {
			rows.push_back(action.Label);
		}
		rows.push_back("RESUME");
		rows.push_back("QUIT");

		const int textLeft = left + 12 * scale;
		int y = top + line;
		engine::render::DebugText::Draw(image, textLeft, y, "SETTINGS", 130, 205, 255, scale);
		y += line * 2;
		for (size_t index = 0; index < rows.size(); index++) {
			const bool active = index == menu.Selected(actions.size());
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
