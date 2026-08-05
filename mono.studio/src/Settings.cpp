#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/ContentSources.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	// The same string `Interface.cpp` docks and focuses. Declared here rather
	// than shared through a header because two translation units naming one
	// window is exactly the drift `###` is guarding against — so it is written
	// once in each and checked by the panel appearing where it was left.
	static constexpr const char *PREFERENCES = "Preferences###Studio Settings";

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

	void Editor::DrawGeneralSettings() {
		// **What the editor does, as opposed to what it looks like.** Splitting
		// these off the Appearance page is not tidiness: a page called
		// Appearance holding the world lifecycle and the frame pacing is a page
		// nobody looks in for either of them.
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

		ImGui::SeparatorText("Frames");

		// **Vertical sync, then a cap, and the cap only matters without it.**
		// The two are one decision presented as two controls because they fail
		// differently: sync ties the frame to the display and is what anybody
		// wants by default; turning it off is what a benchmark or a latency
		// measurement needs, and *then* the question of a ceiling arises.
		if (ImGui::Checkbox("Vertical sync", &VerticalSync)) {
			if (Renderer.SetVerticalSync(VerticalSync)) {
				Say(VerticalSync ? "frames are paced by the display" : "vertical sync off");
			} else {
				// **Put back rather than left lying.** A checkbox that stays
				// ticked while the device ignored it is a control reporting a
				// state the program is not in — and the driver refusing is an
				// ordinary outcome rather than a fault.
				VerticalSync = !VerticalSync;
				Say("this device will not change vertical sync", engine::core::LogLevel::Warning);
			}
		}

		ImGui::BeginDisabled(VerticalSync);

		bool capped = FrameCap > 0.0f;
		if (ImGui::Checkbox("Limit frame rate", &capped)) {
			FrameCap = capped ? 120.0f : 0.0f;
		}

		ImGui::BeginDisabled(!capped);
		ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));
		ImGui::SliderFloat("Frames per second", &FrameCap, 30.0f, 360.0f, "%.0f fps");
		ImGui::EndDisabled();

		ImGui::EndDisabled();

		ImGui::TextDisabled("without a cap the editor draws as fast as it can, which on a still");
		ImGui::TextDisabled("scene is a great many frames of the same picture");

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

	void Editor::DrawContentSettings() {
		// **The order is the feature.** Nothing in the engine implements
		// "local cache first, then the origin next door, then the one across
		// the internet" — that is what a list in this order *means* to
		// `delivery::AssetClient`, which walks it and stops at the first source
		// that answers. So the page is a reorderable list, and the arrows are
		// the policy editor.
		ImGui::SeparatorText("Origins, in the order they are tried");

		ImGui::TextDisabled("the first source that answers wins; one that fails is passed over");

		bool changed = false;

		if (ImGui::BeginTable("##sources", 6, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(24.0f));
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.25f);
			ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(96.0f));
			ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.5f);
			ImGui::TableSetupColumn("##order", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(52.0f));
			ImGui::TableSetupColumn("##drop", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(24.0f));
			ImGui::TableHeadersRow();

			int move = 0;
			size_t moveFrom = 0;
			int remove = -1;

			for (size_t index = 0; index < Content.Sources.size(); ++index) {
				engine::delivery::Source &source = Content.Sources[index];
				// **The id is pinned to the row, not to the text.** A widget's
				// id comes from its label, so a field whose label changed as
				// somebody typed into it would become a different widget and
				// drop keyboard focus after one character — the lesson the
				// script editor's tabs already carry.
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				changed |= ImGui::Checkbox("##enabled", &source.Enabled);

				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed |= TextField("##name", source.Name);

				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				int kind = source.Kind == engine::delivery::SourceKind::Directory ? 0 : 1;
				if (ImGui::Combo("##kind", &kind, "directory\0http\0")) {
					source.Kind = kind == 0 ? engine::delivery::SourceKind::Directory
											: engine::delivery::SourceKind::Http;
					changed = true;
				}

				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed |= TextField(
					"##location",
					source.Location,
					source.Kind == engine::delivery::SourceKind::Http ? "127.0.0.1:9080" : "path/to/store"
				);
				if (!source.IsValid() && ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						source.Kind == engine::delivery::SourceKind::Http
							? "an address and a port, as 127.0.0.1:9080 — a host name has to be resolved first"
							: "a directory holding a published content store"
					);
				}

				ImGui::TableNextColumn();
				if (ImGui::SmallButton("^")) {
					move = -1;
					moveFrom = index;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("v")) {
					move = 1;
					moveFrom = index;
				}

				ImGui::TableNextColumn();
				if (ImGui::SmallButton("x")) {
					remove = static_cast<int>(index);
				}

				ImGui::PopID();
			}
			ImGui::EndTable();

			// Applied after the loop, because both mutate the vector the loop
			// is walking.
			if (move != 0) {
				changed |= Content.Move(moveFrom, move);
			}
			if (remove >= 0) {
				Content.Sources.erase(Content.Sources.begin() + remove);
				changed = true;
			}
		}

		if (ImGui::Button("Add origin")) {
			Content.Sources.push_back(engine::delivery::Source{
				.Name = "origin",
				.Kind = engine::delivery::SourceKind::Http,
				.Location = "127.0.0.1:" + std::to_string(engine::delivery::DEFAULT_ORIGIN_PORT),
				.Enabled = true,
			});
			changed = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Add local store")) {
			Content.Sources.push_back(engine::delivery::Source{
				.Name = "on-disk",
				.Kind = engine::delivery::SourceKind::Directory,
				.Location = "",
				.Enabled = true,
			});
			changed = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset to default")) {
			Content = ContentSources::Default();
			changed = true;
		}

		ImGui::SeparatorText("Trust");

		// **Without this nothing is fetched, and saying so here is the point.**
		// A client that accepted an unsigned manifest would have no trust
		// boundary at all, and that failure is invisible until somebody is
		// serving content the publisher did not write.
		changed |= TextField("Publisher key", Content.PublisherKey, "64 hex characters");
		if (Content.PublisherKey.empty()) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"no publisher key — nothing will be fetched, because nothing could be verified"
			);
		} else if (!engine::assets::PublicKey::FromHex(Content.PublisherKey)) {
			ImGui::TextDisabled("64 lowercase hex characters; `cdn --publish` prints it");
		}

		ImGui::SeparatorText("Cache");

		std::string cache = Content.CachePath.generic_string();
		if (TextField("Cache directory", cache, "empty keeps nothing")) {
			Content.CachePath = std::filesystem::path(cache);
			changed = true;
		}
		ImGui::TextDisabled("verified content kept between sessions; empty keeps none");

		if (changed) {
			// Saved as it is edited rather than behind an Apply button. There
			// is nothing here that is only valid as a set, so a half-finished
			// list is a half-finished list either way — and an editor that lost
			// a typed address on exit is worse than one that saved it.
			Content.Save(ContentSourcesPath);
		}
	}

	void Editor::DrawSettings() {
		if (!ShowSettings) {
			return;
		}

		if (!ImGui::Begin(PREFERENCES, &ShowSettings)) {
			ImGui::End();
			return;
		}

		// **Pages rather than one long panel.** Appearance is read once and
		// forgotten; keybinds are a table somebody searches. Stacking them
		// would put a scroll bar between a person and whichever one they came
		// for.
		if (ImGui::BeginTabBar("##settings")) {
			if (ImGui::BeginTabItem("General")) {
				DrawGeneralSettings();
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Appearance")) {
				DrawAppearanceSettings();
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Content")) {
				DrawContentSettings();
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
