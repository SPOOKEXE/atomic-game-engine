#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	namespace {
		using engine::ui::Palette;

		// A swatch of what a palette looks like, drawn rather than styled.
		//
		// **The point of a theme picker is to show the theme.** A list of seven
		// words makes somebody try all seven to find out what "Shadow" is; a
		// row of colours answers it before they click. The swatch has to be
		// drawn because imgui's style holds one palette at a time — the live
		// one — so the other six exist only as numbers until something paints
		// them.
		void Swatch(const engine::ui::PaletteSample &sample, float size) {
			ImDrawList *draw = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float rounding = engine::ui::Scaled(engine::ui::Radius::Control);

			// Surface, then the raised face over it, then the accent: the three
			// relationships the palette is actually judged on.
			draw->AddRectFilled(
				origin, ImVec2(origin.x + size * 2.0f, origin.y + size), sample.Surface, rounding
			);
			draw->AddRectFilled(
				ImVec2(origin.x + size * 0.18f, origin.y + size * 0.22f),
				ImVec2(origin.x + size * 0.92f, origin.y + size * 0.78f),
				sample.Raised,
				rounding
			);
			draw->AddRectFilled(
				ImVec2(origin.x + size * 1.08f, origin.y + size * 0.22f),
				ImVec2(origin.x + size * 1.82f, origin.y + size * 0.78f),
				sample.Accent,
				rounding
			);

			ImGui::Dummy(ImVec2(size * 2.0f, size));
		}
	}

	void Editor::DrawSettings() {
		if (!ShowSettings) {
			return;
		}

		if (!ImGui::Begin("Settings", &ShowSettings)) {
			ImGui::End();
			return;
		}

		ImGui::SeparatorText("Theme");

		// **Applied on the click and not on an OK button.** A theme is judged
		// by looking at it, and a picker that needed confirming would make
		// comparing two of them a four-click job. There is nothing to cancel:
		// the previous palette is one click away and the choice is a
		// preference, not an edit to the game.
		const Palette current = engine::ui::CurrentPalette();

		for (size_t index = 0; index < engine::ui::PALETTE_COUNT; index++) {
			const auto palette = static_cast<Palette>(index);
			const bool chosen = palette == current;

			ImGui::PushID(static_cast<int>(index));

			const float row = engine::ui::Scaled(engine::ui::Size::Row);
			// `SetPalette` restyles and marks the layout dirty itself, so this
			// is the whole of choosing a theme.
			if (ImGui::Selectable("##palette", chosen, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, row))) {
				engine::ui::SetPalette(palette);
			}

			ImGui::SameLine();
			Swatch(engine::ui::SampleOf(palette), row * 0.82f);

			ImGui::SameLine();
			ImGui::TextUnformatted(engine::ui::Describe(palette));

			ImGui::PopID();
		}

		ImGui::Spacing();
		ImGui::TextDisabled("remembered in the layout file, beside the panel positions");

		ImGui::SeparatorText("Interface");

		// The scale the whole interface is built from. Not applied live on
		// every drag frame: `ApplyEditorTheme` rebuilds every metric and the
		// fonts are rasterised at a fixed size, so a scale changed mid-drag
		// would rebuild the style sixty times a second to show text that cannot
		// follow it until the next start.
		float scale = Settings.Scale;
		ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));
		if (ImGui::SliderFloat("Scale", &scale, 0.75f, 2.0f, "%.2fx")) {
			Settings.Scale = scale;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			engine::ui::ApplyEditorTheme(Settings.Scale);
			Say("interface scale is now " + std::to_string(Settings.Scale) +
				"x — restart to rasterise the fonts at it");
		}

		ImGui::SeparatorText("Simulation");

		// Read-only here on purpose: a world's tick rate is `WorldSettings` and
		// is decided when the world is created, so a control that looked
		// editable and changed nothing would be worse than a number.
		ImGui::Text("tick rate: %.0f Hz", Settings.TickRate);
		ImGui::TextDisabled("a world's rate is set when it is created");

		ImGui::End();
	}
}
