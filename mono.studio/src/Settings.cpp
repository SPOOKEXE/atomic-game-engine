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
	// window is exactly the drift `###` is guarding against - so it is written
	// once in each and checked by the panel appearing where it was left.
	static constexpr const char *PREFERENCES = "Preferences###Studio Settings";

	namespace {
		using engine::ui::Palette;

		// A swatch of what a palette looks like, drawn rather than styled.
		//
		// **The point of a theme picker is to show the theme.** A list of seven
		// words makes somebody try all seven to find out what "Shadow" is; a
		// row of colours answers it before they click. The swatch has to be
		// drawn because imgui's style holds one palette at a time - the live
		// one - so the other six exist only as numbers until something paints
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

		// One editable colour, and the cross that gives it back to the theme.
		//
		// **The picker always shows a colour, and the label says whose it is.**
		// A row that was blank until somebody chose something would make the
		// seven defaults invisible - and the first thing anybody wants when
		// recolouring is the colour they are departing from, to nudge it.
		//
		// @param colours  The set being edited.
		// @param which    Which colour.
		// @param inherited What this colour is when nothing overrides it.
		// @return Whether the set changed.
		bool
		ColourRow(engine::ui::ThemeColours &colours, engine::ui::ThemeColour which, unsigned int inherited) {
			const bool overridden = colours[which].has_value();
			const unsigned int packed = overridden ? *colours[which] : inherited;

			ImGui::PushID(static_cast<int>(which));

			bool changed = false;
			ImVec4 value = ImGui::ColorConvertU32ToFloat4(packed);

			// **`NoInputs`, so the row is a swatch and not four number fields.**
			// The picker behind the swatch has the numbers, and the hex field
			// in it, for anybody who arrived with a colour rather than a
			// feeling.
			if (ImGui::ColorEdit4("##colour", &value.x, ImGuiColorEditFlags_NoInputs)) {
				colours[which] = ImGui::ColorConvertFloat4ToU32(value);
				changed = true;
			}

			ImGui::SameLine();
			ImGui::TextUnformatted(engine::ui::Describe(which));

			// **The reset is only there when there is something to reset.** A
			// permanently visible cross beside seven rows that are all
			// inherited is seven invitations to press something that does
			// nothing.
			if (overridden) {
				ImGui::SameLine();
				ImGui::TextDisabled("·");
				ImGui::SameLine();
				if (ImGui::SmallButton("reset")) {
					colours[which].reset();
					changed = true;
				}
			}

			ImGui::PopID();
			return changed;
		}

		// A panel's title as somebody reads it, which is the part before `###`.
		//
		// imgui keys a window on the whole label and hides the id half from the
		// title bar; a settings page that listed the raw string would be the one
		// place in the editor showing "Preferences###Studio Settings".
		std::string PanelLabel(const char *title) {
			const std::string_view whole(title);
			const size_t marker = whole.find("###");
			return std::string(marker == std::string_view::npos ? whole : whole.substr(0, marker));
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
		// is for - but an author debugging a world that keeps closing under
		// them needs to be able to stop it happening rather than work out why.
		ImGui::Checkbox("Close empty worlds automatically", &AutoManageWorlds);

		ImGui::BeginDisabled(!AutoManageWorlds);
		ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));

		// **Three answers rather than a number with two special values**, which
		// is `world::IdleSleep`'s whole argument: "this world runs forever" is a
		// decision somebody pays for, and it should not be reachable by dragging
		// a slider to its end.
		static const char *MODES[] = {"After a while", "As soon as it is empty", "Never"};
		int mode = static_cast<int>(IdleSleepMode);
		if (ImGui::Combo("Close empty worlds", &mode, MODES, IM_ARRAYSIZE(MODES))) {
			IdleSleepMode = static_cast<engine::world::IdleSleep>(mode);
		}

		ImGui::BeginDisabled(IdleSleepMode != engine::world::IdleSleep::Timeout);
		ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));

		// **The slider stops where the policy does.** `DecideLifecycle` clamps
		// to ten minutes, so a control offering thirty would be offering a
		// number that is silently ignored - which reads as the setting being
		// broken rather than as the ceiling being deliberate.
		constexpr float MAXIMUM_MINUTES =
			static_cast<float>(engine::world::MAXIMUM_IDLE_LIMIT_SECONDS) / 60.0f;

		float minutes = IdleCloseSeconds / 60.0f;
		if (ImGui::SliderFloat("Close after", &minutes, 0.5f, MAXIMUM_MINUTES, "%.1f min")) {
			IdleCloseSeconds = minutes * 60.0f;
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		if (IdleSleepMode == engine::world::IdleSleep::Never) {
			ImGui::TextDisabled("every world keeps ticking, so NPCs and clocks keep running -");
			ImGui::TextDisabled("and every world keeps costing what it costs");
		} else {
			ImGui::TextDisabled("a world with no player and nobody looking at it stops ticking;");
			ImGui::TextDisabled("teleporting into a closed world opens it first");
		}

		ImGui::SeparatorText("Frames");

		// **Vertical sync, then a cap, and the cap only matters without it.**
		// The two are one decision presented as two controls because they fail
		// differently: sync ties the frame to the display, which costs a whole
		// refresh of latency between the mouse and the viewport, so the editor
		// ships with it off and a 120 fps ceiling instead. Ticking it back on is
		// for a viewport that tears; the ceiling is what keeps a still scene
		// from spinning the fans once it is off.
		// **Asked for from here and applied at the top of the next frame**, which
		// is the renderer's business and is worth knowing about at the one call
		// site that clicks it mid-frame. This panel is drawn *after*
		// `Renderer::WaitForFrame` has already acquired this frame's swapchain
		// image; setting the mode here used to rebuild the swapchain underneath
		// it and the frame then presented a freed texture. See
		// `render::Renderer::SetVerticalSync`.
		if (ImGui::Checkbox("Vertical sync", &VerticalSync)) {
			if (Renderer.SetVerticalSync(VerticalSync)) {
				Say(VerticalSync ? "frames are paced by the display" : "vertical sync off");
			} else {
				// **Put back rather than left lying.** A checkbox that stays
				// ticked while the device ignored it is a control reporting a
				// state the program is not in - and the driver refusing is an
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

		// **Four rates under the one ceiling, because a still editor and a busy
		// one are not the same question.** The number above is the most this
		// program will ever draw; these four are what it settles to when nobody
		// is asking it for anything. An editor behind a browser drawing a
		// hundred and twenty identical pictures a second is what they exist to
		// stop.
		ImGui::Spacing();
		ImGui::SeparatorText("Rates");

		ImGui::TextDisabled("the lowest ceiling that applies is the one the frame is paced at");

		const auto rate = [](const char *label, float &value, const char *tip) {
			ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));
			ImGui::SliderFloat(label, &value, 0.0f, 360.0f, value <= 0.0f ? "no limit" : "%.0f Hz");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", tip);
			}
		};

		rate(
			"Interface, active",
			InterfaceActiveHz,
			"How often the panels are rebuilt while somebody is working."
		);
		rate(
			"Interface, idle",
			InterfaceIdleHz,
			"After three seconds with no key and no mouse. Drop this to stop a still\n"
			"editor costing a laptop its fans."
		);
		rate(
			"Renderer, focused",
			RendererFocusedHz,
			"How often the world behind the panels is redrawn while this window has focus."
		);
		rate(
			"Renderer, unfocused",
			RendererUnfocusedHz,
			"While the window is behind something else. There is nobody looking at it,\n"
			"so this is the one worth setting low."
		);

		ImGui::EndDisabled();

		ImGui::EndDisabled();

		ImGui::TextDisabled("without a cap the editor draws as fast as it can, which on a still");
		ImGui::TextDisabled("scene is a great many frames of the same picture");

		// **Said rather than left implied.** `Renderer::Render` owns the
		// swapchain, the interface and the present in one call, so a frame that
		// redraws the panels redraws the world too - the interface and renderer
		// rates therefore bound the same frame rather than running apart. A page
		// offering four independent numbers that are not independent would be
		// worse than one saying so.
		ImGui::TextDisabled("interface and renderer share one frame today, so the two act as");
		ImGui::TextDisabled("ceilings on it rather than as separate clocks");
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
			if (ImGui::Selectable(
					"##palette", chosen, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, row)
				)) {
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

		DrawThemeColours();
		DrawPanelColours();

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
				"x - restart to rasterise the fonts at it");
		}
	}

	void Editor::DrawThemeColours() {
		ImGui::SeparatorText("Colours");

		// **Over the palette, not instead of it**, and this line is the whole
		// mental model: choosing a theme still works after somebody has picked
		// an accent, because only the accent is pinned. Without the sentence,
		// the first person to override a colour and then switch theme reads it
		// as a bug.
		ImGui::TextDisabled("chosen over whichever theme is selected - the rest still follows it");
		ImGui::Spacing();

		engine::ui::ThemeColours chosen = engine::ui::GlobalColours();
		const engine::ui::Palette palette = engine::ui::CurrentPalette();

		// **Both colour sections are the same seven rows in one window**, so
		// `ColourRow`'s own id - the colour it edits - repeats between them.
		// imgui keys a widget on the whole stack, and two swatches sharing an id
		// is two swatches that drag each other. The scope is pushed here rather
		// than inside `ColourRow` because the row cannot know which section it
		// is in, and a caller that forgot would get a conflict rather than a
		// compile error.
		ImGui::PushID("theme");

		bool changed = false;
		for (size_t index = 0; index < engine::ui::THEME_COLOUR_COUNT; index++) {
			const auto which = static_cast<engine::ui::ThemeColour>(index);
			changed |= ColourRow(chosen, which, engine::ui::ColourOf(palette, which));
		}
		ImGui::PopID();

		if (chosen.Any()) {
			ImGui::Spacing();
			if (ImGui::Button("Reset all")) {
				chosen.Clear();
				changed = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("back to the palette, every colour");
		}

		if (changed) {
			// Restyles and persists in one call, like `SetPalette` - see its
			// note for why neither is left to the caller.
			engine::ui::SetGlobalColours(chosen);
		}
	}

	void Editor::DrawPanelColours() {
		ImGui::SeparatorText("One panel");

		ImGui::TextDisabled("a panel of its own colour, over the theme above");
		ImGui::Spacing();

		const std::span<const char *const> panels = SkinnablePanels();
		if (panels.empty()) {
			return;
		}

		ColourPanel = std::clamp(ColourPanel, 0, static_cast<int>(panels.size()) - 1);
		const char *panel = panels[static_cast<size_t>(ColourPanel)];

		ImGui::SetNextItemWidth(engine::ui::Scaled(220.0f));
		if (ImGui::BeginCombo("##panel", PanelLabel(panel).c_str())) {
			for (size_t index = 0; index < panels.size(); index++) {
				const bool selected = static_cast<int>(index) == ColourPanel;

				// **A dot beside the ones that carry a colour**, so a person
				// who set one three panels ago can find it again. Without it
				// the only way to audit what has been recoloured is to walk
				// twenty-odd entries one at a time.
				const bool carries = Prefs.PanelColours.count(panels[index]) != 0;
				const std::string label =
					PanelLabel(panels[index]) + (carries ? std::string("  ·") : std::string());

				if (ImGui::Selectable(label.c_str(), selected)) {
					ColourPanel = static_cast<int>(index);
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::Spacing();

		// **Looked up rather than inserted here.** `operator[]` on the map would
		// give every panel somebody merely *looked at* an empty entry, and an
		// empty entry is a line in `preferences.json` claiming a colour that is
		// not there.
		const auto found = Prefs.PanelColours.find(panel);
		engine::ui::ThemeColours chosen =
			found == Prefs.PanelColours.end() ? engine::ui::ThemeColours{} : found->second;

		// Scoped apart from the section above, which draws the same seven rows
		// in the same window. See the note there.
		ImGui::PushID("panel");

		bool changed = false;
		for (size_t index = 0; index < engine::ui::THEME_COLOUR_COUNT; index++) {
			const auto which = static_cast<engine::ui::ThemeColour>(index);

			// The colour it inherits is the *live* one - the palette with the
			// editor's overrides already on it - because that is what this
			// panel is actually drawn in today.
			changed |= ColourRow(chosen, which, engine::ui::ColourOf(which));
		}
		ImGui::PopID();

		if (chosen.Any()) {
			ImGui::Spacing();
			if (ImGui::Button("Clear this panel")) {
				chosen.Clear();
				changed = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("back to the theme");
		}

		if (!changed) {
			return;
		}

		if (chosen.Any()) {
			Prefs.PanelColours[panel] = chosen;
		} else {
			Prefs.PanelColours.erase(panel);
		}

		// **Nothing to restyle and nothing to mark dirty.** A panel's colours
		// are pushed as it draws rather than installed in the style, so the
		// next frame is already right; and they live in `preferences.json`,
		// which `SaveConfiguration` writes on the way out.
	}

	void Editor::DrawKeybindSettings() {
		// **The table is the source of truth, not a picture of one.**
		// `DrawShortcuts` reads exactly these rows, so a key changed here is
		// changed everywhere - including the labels in the menus, which ask the
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
									said += " - taken from " + std::string(other.Name);
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
		ImGui::TextUnformatted("not saved yet - bindings last for this run");
		ImGui::PopStyleColor();
	}

	void Editor::DrawContentSettings() {
		// **The order is the feature.** Nothing in the engine implements
		// "local cache first, then the origin next door, then the one across
		// the internet" - that is what a list in this order *means* to
		// `delivery::AssetClient`, which walks it and stops at the first source
		// that answers. So the page is a reorderable list, and the arrows are
		// the policy editor.
		ImGui::SeparatorText("Origins, in the order they are tried");

		ImGui::TextDisabled("the first source that answers wins; one that fails is passed over");

		// **One order, and each row says which directions it is in.** That is
		// the whole of "one server takes the writes, another serves the reads":
		// a read walks this list skipping the write-only rows and stops at the
		// first that answers, and an upload walks it skipping the read-only
		// rows and sends to *every* one. Two lists would be two orderings that
		// drift, and what somebody wants in both directions is the same thing -
		// nearest first.
		ImGui::TextDisabled("a write origin is never fetched from, and a read origin is never uploaded to");

		bool changed = false;

		if (ImGui::BeginTable("##sources", 8, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(24.0f));
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.2f);
			ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(90.0f));
			ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(80.0f));
			ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("Ingest key", ImGuiTableColumnFlags_WidthStretch, 0.25f);
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
				// drop keyboard focus after one character - the lesson the
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
				int role = static_cast<int>(source.Role);
				if (ImGui::Combo("##role", &role, "both\0read\0write\0")) {
					source.Role = static_cast<engine::delivery::SourceRole>(role);
					changed = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"both: fetched from and uploaded to\n"
						"read: fetched from only\n"
						"write: uploaded to only - never consulted for a fetch, because it\n"
						"holds content no signed manifest describes yet"
					);
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
							? "an address and a port, as 127.0.0.1:9080 - a host name has to be resolved "
							  "first"
							: "a directory holding a published content store"
					);
				}

				ImGui::TableNextColumn();

				// **Only a write row needs one, and a directory never does.**
				// Writing to a directory is writing to a filesystem this process
				// already has; the key exists to admit a request to somebody
				// else's origin, and there is no request here to admit.
				const bool needsKey = source.Role != engine::delivery::SourceRole::Read &&
									  source.Kind == engine::delivery::SourceKind::Http;
				if (needsKey) {
					ImGui::SetNextItemWidth(-FLT_MIN);

					// **Masked, but saved** - unlike the signing seed on the
					// Assets panel, which is asked for every time and never
					// kept. `ContentSources.hpp` carries why those two differ:
					// an ingest key buys disk, a signing seed buys trust.
					changed |= TextField("##ingest", source.IngestKey, "shared secret", true);
					if (source.IngestKey.empty() && ImGui::IsItemHovered()) {
						ImGui::SetTooltip(
							"without this the origin refuses every upload, so this row is not a write target"
						);
					}
				} else {
					ImGui::TextDisabled("-");
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
			Content.Sources.push_back(
				engine::delivery::Source{
					.Name = "origin",
					.Kind = engine::delivery::SourceKind::Http,
					.Location = "127.0.0.1:" + std::to_string(engine::delivery::DEFAULT_ORIGIN_PORT),
					.Enabled = true,
				}
			);
			changed = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Add local store")) {
			Content.Sources.push_back(
				engine::delivery::Source{
					.Name = "on-disk",
					.Kind = engine::delivery::SourceKind::Directory,
					.Location = "",
					.Enabled = true,
				}
			);
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
				"no publisher key - nothing will be fetched, because nothing could be verified"
			);
		} else if (!engine::assets::PublicKey::FromHex(Content.PublisherKey)) {
			ImGui::TextDisabled("64 lowercase hex characters; `cdn --publish` prints it");
		}

		ImGui::SeparatorText("Raw folders");

		// **Not origins, and the page has to say why.** Everything above is a
		// place content is *fetched* from, and a fetch is only ever of something
		// a signed manifest names. A folder of PNGs has neither, so these are
		// baked by this editor for this editor - `ContentSources::RawFolders`
		// carries the argument, and the assets panel repeats it on the tab.
		ImGui::TextDisabled("art folders this editor bakes from directly; nothing here reaches a client");

		if (ImGui::Checkbox("Memory-only", &Content.MemoryOnly)) {
			changed = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled(
			Content.MemoryOnly ? "baked results are kept in this process and written nowhere"
							   : "baked results are also written into the store's baked/, ready to publish"
		);

		int dropFolder = -1;
		for (size_t index = 0; index < Content.RawFolders.size(); ++index) {
			ImGui::PushID(static_cast<int>(index + Content.Sources.size()));

			std::string folder = Content.RawFolders[index].generic_string();
			ImGui::SetNextItemWidth(-engine::ui::Scaled(40.0f));
			if (TextField("##rawfolder", folder, "a folder of unprocessed art")) {
				Content.RawFolders[index] = std::filesystem::path(folder);
				changed = true;
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				dropFolder = static_cast<int>(index);
			}
			ImGui::PopID();
		}

		// After the loop, because it mutates the vector the loop is walking.
		if (dropFolder >= 0) {
			Content.RawFolders.erase(Content.RawFolders.begin() + dropFolder);
			changed = true;
		}

		if (ImGui::Button("Add raw folder")) {
			Content.RawFolders.emplace_back();
			changed = true;
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
			// list is a half-finished list either way - and an editor that lost
			// a typed address on exit is worse than one that saved it.
			Content.Save(ContentSourcesPath);

			// **And the clients are rebuilt with it**, which is what makes this
			// page a settings page rather than a form. Before there was
			// anything downstream of it, a wrong key and a working one looked
			// identical here and stayed that way until somebody restarted.
			//
			// Rebuilt on every keystroke, which sounds wasteful and is not:
			// building a client resolves addresses and opens a cache, and both
			// are cheap next to the fetch nothing has asked for yet. The
			// alternative - rebuilding when the field loses focus - makes the
			// Network panel disagree with this one for as long as a cursor sits
			// in a field.
			RebuildContentClients();
		}
	}

	void Editor::DrawDefaultWorldSettings() {
		ImGui::SeparatorText("Worlds a new game opens with");

		ImGui::TextDisabled(
			"ticked worlds are created by File > New. Nothing here touches a game already open."
		);

		ImGui::Spacing();

		// **Two buttons rather than a right-click menu on the list**, because
		// the two things anybody wants from a thirteen-row checkbox page are
		// "all of them" and "none of them", and both are one click away or they
		// are not worth having.
		if (ImGui::SmallButton("Select all")) {
			for (const DefaultWorldEntry &entry : DefaultWorldCatalogue()) {
				SetDefaultWorldEnabled(entry.Key, true);
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Select none")) {
			for (const DefaultWorldEntry &entry : DefaultWorldCatalogue()) {
				SetDefaultWorldEnabled(entry.Key, false);
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset")) {
			for (const DefaultWorldEntry &entry : DefaultWorldCatalogue()) {
				SetDefaultWorldEnabled(entry.Key, entry.OnByDefault);
			}
		}

		ImGui::Spacing();

		size_t chosen = 0;
		for (const DefaultWorldEntry &entry : DefaultWorldCatalogue()) {
			ImGui::PushID(entry.Key.data(), entry.Key.data() + entry.Key.size());

			bool enabled = DefaultWorldEnabled(entry.Key);
			const std::string label(entry.World);
			if (ImGui::Checkbox(label.c_str(), &enabled)) {
				SetDefaultWorldEnabled(entry.Key, enabled);
			}

			// The note is the paragraph that used to sit beside this world's
			// `AddWorld` call in `Editor::NewGame`. A tooltip rather than a line
			// under each row: thirteen paragraphs stacked is a page nobody reads
			// the top of.
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", std::string(entry.Note).c_str());
			}

			// The scene that builds it, dimmed on the same row - an annotation
			// rather than a second name, which is the Explorer's rule for the
			// class name it puts beside an instance.
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(entry.First.data(), entry.First.data() + entry.First.size());
			ImGui::PopStyleColor();

			chosen += enabled ? 1 : 0;
			ImGui::PopID();
		}

		ImGui::Spacing();

		// **Said rather than left to be discovered on the next New.** An empty
		// selection is allowed - somebody clearing the list means it - and a new
		// game that opened one blank world when they expected none, or none when
		// they expected several, is a surprise worth spending a line on.
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		if (chosen == 0) {
			ImGui::TextUnformatted("nothing ticked - a new game opens one empty world called Start");
		} else {
			ImGui::Text("%zu world(s) - the first is the one selected on open", chosen);
		}
		ImGui::PopStyleColor();
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

			if (ImGui::BeginTabItem("Default Worlds")) {
				DrawDefaultWorldSettings();
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
