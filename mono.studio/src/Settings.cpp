#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
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

	void Editor::DrawAppearanceSettings() {
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

		ImGui::SeparatorText("Worlds");

		// **The lifecycle is a policy, so it has a switch.** A universe of
		// subareas cannot tick all of them, which is what closing empty worlds
		// is for — but an author debugging a world that keeps closing under
		// them needs to be able to stop it happening rather than work out why.
		ImGui::Checkbox("Close empty worlds automatically", &AutoManageWorlds);

		ImGui::BeginDisabled(!AutoManageWorlds);
		ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));

		float minutes = IdleCloseSeconds / 60.0f;
		if (ImGui::SliderFloat("Close after", &minutes, 0.5f, 30.0f, "%.1f min")) {
			IdleCloseSeconds = minutes * 60.0f;
		}
		ImGui::EndDisabled();

		ImGui::TextDisabled("a world with no player and nobody looking at it stops ticking;");
		ImGui::TextDisabled("teleporting into a closed world opens it first");

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
	}

	void Editor::DrawKeybindSettings() {
		// **The table is the source of truth, not a picture of one.**
		// `DrawShortcuts` reads exactly these rows, so a key changed here is
		// changed everywhere — including the labels in the menus, which ask the
		// same table what to print. A settings page that listed keys some other
		// code had hard-coded would be a page that lies the first time somebody
		// edits one of them.
		ImGui::SetNextItemWidth(-1.0f);
		TextField("##keybind-filter", KeybindFilter, "Search Actions");

		ImGui::Spacing();

		constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
										  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

		const float height = ImGui::GetContentRegionAvail().y - engine::ui::Scaled(engine::ui::Size::Bar);

		if (ImGui::BeginTable("##keybinds", 4, FLAGS, ImVec2(0.0f, std::max(height, 0.0f)))) {
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.26f);
			ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.22f);
			ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 0.16f);
			ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.36f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (Keybind &binding : Keybinds::All()) {
				int score = 0;
				if (!FuzzyMatch(KeybindFilter, binding.Name, score) &&
					!FuzzyMatch(KeybindFilter, binding.Description, score)) {
					continue;
				}

				const auto index = static_cast<int>(binding.Bound);
				const bool rebinding = RebindingAction == index;

				ImGui::PushID(index);
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(binding.Name);

				// --- the shortcut ---------------------------------------------

				ImGui::TableSetColumnIndex(1);

				if (rebinding) {
					// **Captured from the next key, not typed into a field.**
					// Nobody knows how to spell `Ctrl+Shift+S` in a way a
					// parser would accept, and asking them to is how a
					// rebinding UI becomes one nobody uses.
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
					ImGui::TextUnformatted("press a key…");
					ImGui::PopStyleColor();

					if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
						RebindingAction = -1;
					} else if (const Chord pressed = Keybinds::Pressed(); pressed.IsBound()) {
						// **Says what it is about to take, before taking it.**
						// `Set` unbinds the loser either way; without this the
						// only evidence is a shortcut that quietly stopped
						// working somewhere else in the editor.
						const Action holder = Keybinds::Holder(pressed, binding.Bound);

						Keybinds::Set(binding.Bound, pressed);
						RebindingAction = -1;

						std::string said = std::string(binding.Name) + " is now " + pressed.Text();
						if (holder != Action::Count) {
							for (const Keybind &other : Keybinds::All()) {
								if (other.Bound == holder) {
									said += " — taken from " + std::string(other.Name);
									break;
								}
							}
						}
						Say(said);
					}
				} else {
					const std::string text = binding.Keys.Text();
					const bool bound = binding.Keys.IsBound();

					if (!bound) {
						ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
					}

					if (ImGui::Selectable(bound ? text.c_str() : "unbound##none", false)) {
						RebindingAction = index;
					}

					if (!bound) {
						ImGui::PopStyleColor();
					}

					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("click to rebind, right-click to clear");
					}

					if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
						Keybinds::Set(binding.Bound, Chord{});
					}
				}

				// **Where it applies, shown rather than left to be discovered.**
				// A binding scoped to the tree that does nothing in a viewport
				// reads as a broken shortcut unless the table says why. See
				// `Keybinds::Scope`.
				ImGui::TableSetColumnIndex(2);
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				switch (binding.Where) {
					case Scope::Global:
						ImGui::TextUnformatted("anywhere");
						break;
					case Scope::Viewport:
						ImGui::TextUnformatted("viewport");
						break;
					case Scope::Tree:
						ImGui::TextUnformatted("explorer");
						break;
					case Scope::Script:
						ImGui::TextUnformatted("scripts");
						break;
				}
				ImGui::PopStyleColor();

				ImGui::TableSetColumnIndex(3);
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted(binding.Description);
				ImGui::PopStyleColor();

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		if (ImGui::Button("Reset to Defaults")) {
			Keybinds::Reset();
			RebindingAction = -1;
			Say("keybinds are back to their defaults");
		}

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());

		// Said out loud rather than discovered. See `Keybinds::Set`.
		ImGui::TextUnformatted("not saved yet — bindings last for this run");
		ImGui::PopStyleColor();
	}

	void Editor::DrawSettings() {
		if (!ShowSettings) {
			return;
		}

		if (!ImGui::Begin("Studio Settings", &ShowSettings)) {
			ImGui::End();
			return;
		}

		// **Pages rather than one long panel.** Appearance is read once and
		// forgotten; keybinds are a table somebody searches. Stacking them
		// would put a scroll bar between a person and whichever one they came
		// for.
		if (ImGui::BeginTabBar("##settings")) {
			if (ImGui::BeginTabItem("Appearance")) {
				DrawAppearanceSettings();
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Keybinds")) {
				DrawKeybindSettings();
				ImGui::EndTabItem();
			} else {
				// A rebind left half-finished on a page nobody is looking at is
				// a key that binds itself to whatever is typed next.
				RebindingAction = -1;
			}

			ImGui::EndTabBar();
		}

		ImGui::End();
	}
}
