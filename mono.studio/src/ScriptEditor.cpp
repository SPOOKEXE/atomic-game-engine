#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Store;

	void Editor::DrawScripts() {
		if (!ShowScripts) {
			return;
		}

		if (!ImGui::Begin("Script Editor", &ShowScripts)) {
			ImGui::End();
			return;
		}

		if (Scripts.empty()) {
			ImGui::TextDisabled("no script open");
			ImGui::TextDisabled("double-click a Script in the explorer, or insert one");
			ImGui::End();
			return;
		}

		size_t closing = Scripts.size();

		if (ImGui::BeginTabBar("##scripts", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_TabListPopupButton)) {
			for (size_t index = 0; index < Scripts.size(); index++) {
				OpenScript &tab = Scripts[index];

				// The instance's *current* name, read every frame rather than
				// cached. A cached one is wrong for one frame after a rename,
				// and one frame is enough to be seen — `ui/AGENTS.md` states
				// that rule and this is the panel most tempted to break it.
				std::string label = "(deleted)";
				bool alive = false;

				if (tab.World.IsValid()) {
					Universe->Enter(tab.World, [&](Store &store) {
						if (!store.Alive(tab.Instance)) {
							return;
						}
						alive = true;
						const Name name = store.InstanceNameOf(tab.Instance);
						label = name.IsValid() ? std::string(Label(name)) : std::string("Script");
					});
				}

				if (tab.Modified) {
					label += " *";
				}

				// **`###` pins the tab's id to the instance, and this is not a
				// nicety.** imgui derives a widget's id from its label, so appending
				// the modified marker made the tab a *different tab* the instant
				// anybody typed — which closed the old one, rebuilt the field inside
				// it, and dropped keyboard focus after exactly one character. The
				// symptom was a script editor that took the first keystroke and
				// ignored every one after it, and it took a screenshot to see.
				label += "###";
				label += std::to_string(tab.Instance.Id);

				ImGui::PushID(static_cast<int>(index));

				bool open = true;
				if (ImGui::BeginTabItem(label.c_str(), &open)) {
					ActiveScript = static_cast<int>(index);

					if (!alive) {
						ImGui::TextDisabled("the script this tab was editing has been deleted");
					} else {
						ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
						ImGui::Text(
							"%s   in %s",
							tab.Path.IsValid() ? Label(tab.Path) : "(unsaved — a path is chosen on save)",
							Label(Universe->NameOf(tab.World))
						);
						ImGui::PopStyleColor();

						ImGui::SameLine();
						if (ImGui::SmallButton("Save")) {
							SaveScriptTab(tab);
						}

						// This tab's own scene: a script in a world being edited is
						// just text, whatever another scene is doing.
						if (ModeOf(tab.World) != RunMode::Edit) {
							ImGui::SameLine();
							ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
							// **Said plainly, because the alternative is a
							// mystery.** A running game already started its
							// scripts; editing the text now changes what runs
							// the *next* time, exactly as it does in Roblox, and
							// an editor that let somebody type into a live
							// script without saying so produces "my change did
							// nothing".
							ImGui::TextUnformatted("edits apply on the next run");
							ImGui::PopStyleColor();
						}

						// The zoom, beside the state it applies to. A control
						// nobody can find is a control that only exists for
						// people who already knew the wheel did something.
						ImGui::SameLine();
						ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
						ImGui::Text("   %.0f%%", static_cast<double>(ScriptZoom * 100.0f));
						ImGui::PopStyleColor();

						ImGui::SameLine();
						if (ImGui::SmallButton("-")) {
							ScriptZoom = std::clamp(ScriptZoom - 0.1f, 0.6f, 3.0f);
						}
						ImGui::SameLine();
						if (ImGui::SmallButton("+")) {
							ScriptZoom = std::clamp(ScriptZoom + 0.1f, 0.6f, 3.0f);
						}
						ImGui::SameLine();
						if (ImGui::SmallButton("Reset")) {
							ScriptZoom = 1.0f;
						}
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("Ctrl+wheel over the code zooms it");
						}

						ImGui::SameLine();
						if (ImGui::SmallButton(ShowFind ? "Hide Find" : "Find")) {
							ShowFind = !ShowFind;
						}

						ImGui::Separator();

						// **Find and Replace All, and deliberately not Find
						// Next.** Jumping the caret to a match means setting the
						// selection inside `InputTextMultiline`, which imgui
						// does not expose — reaching into `ImGuiInputTextState`
						// to do it would tie the script editor to a private
						// layout that changes between imgui releases. A match
						// count and a whole-file replace are the two thirds of
						// this that can be built honestly.
						if (ShowFind) {
							ImGui::SetNextItemWidth(180.0f * Settings.Scale);
							TextField("##find", FindText, "find");

							ImGui::SameLine();
							ImGui::SetNextItemWidth(180.0f * Settings.Scale);
							TextField("##replace", ReplaceText, "replace with");

							size_t matches = 0;
							if (!FindText.empty()) {
								for (size_t at = tab.Text.find(FindText); at != std::string::npos;
									 at = tab.Text.find(FindText, at + FindText.size())) {
									matches++;
								}
							}

							ImGui::SameLine();
							ImGui::BeginDisabled(matches == 0);
							if (ImGui::SmallButton("Replace All")) {
								std::string rebuilt;
								rebuilt.reserve(tab.Text.size());

								// Built once into a new string rather than
								// replaced in place: replacing in place while
								// scanning re-finds the replacement when it
								// contains the needle, which is an editor that
								// hangs on "a" -> "aa".
								size_t at = 0;
								for (size_t found = tab.Text.find(FindText, at);
									 found != std::string::npos;
									 found = tab.Text.find(FindText, at)) {
									rebuilt.append(tab.Text, at, found - at);
									rebuilt.append(ReplaceText);
									at = found + FindText.size();
								}
								rebuilt.append(tab.Text, at, std::string::npos);

								tab.Text = rebuilt;
								tab.Modified = true;
								Say(
									"replaced " + std::to_string(matches) + " occurrence(s) in " +
									std::string(Label(tab.Path))
								);
							}
							ImGui::EndDisabled();

							ImGui::SameLine();
							ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
							if (FindText.empty()) {
								ImGui::TextUnformatted("type something to find");
							} else {
								ImGui::Text("%zu match(es)", matches);
							}
							ImGui::PopStyleColor();

							ImGui::Separator();
						}

						// **The monospace face, which is what makes this a code
						// editor rather than a text box.** Columns line up, an
						// `l` is not an `I`, and indentation is a width rather
						// than a guess. `mono.studio/AGENTS.md` listed the
						// absence of one as a deferred gap with the reason being
						// a font this repository did not have; it has four now.
						const engine::ui::ScopedFont code(engine::ui::Typeface::Monospace);

						// **Zoom, and it scales the font rather than the
						// interface.** `Options::Scale` rebuilds every metric
						// in the editor and needs a restart to rasterise the
						// faces at the new size; this is one panel's text, and
						// wanting bigger code is not wanting a bigger
						// properties panel. `SetWindowFontScale` stretches the
						// glyphs the atlas already has, which is why it costs
						// nothing and why it goes soft a long way from 1.
						ImGui::SetWindowFontScale(ScriptZoom);

						if (CodeField("##text", tab.Text, -1.0f, -1.0f)) {
							tab.Modified = true;
						}

						// **Restored before the panel ends.** The scale is a
						// property of the window rather than of the widget, so
						// leaving it set would zoom this tab's title and every
						// other thing drawn in the panel after it.
						ImGui::SetWindowFontScale(1.0f);

						// Ctrl+wheel over the text, which is what every editor
						// binds it to. Guarded on hovering the field so that
						// scrolling the tab bar or the panel does not resize
						// the code by accident.
						if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl) {
							if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f) {
								ScriptZoom = std::clamp(ScriptZoom + wheel * 0.1f, 0.6f, 3.0f);
							}
						}

						// Ctrl+S inside the editor saves the *script*, not the
						// game. The menu bar's shortcut is guarded on
						// `WantTextInput`, so the two do not both fire.
						if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
							ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
							SaveScriptTab(tab);
						}
					}

					ImGui::EndTabItem();
				}

				ImGui::PopID();

				if (!open) {
					closing = index;
				}
			}
			ImGui::EndTabBar();
		}

		ImGui::End();

		if (closing < Scripts.size()) {
			// Saved on the way out rather than dropped. An editor that lost
			// what you typed because you closed the tab is an editor nobody
			// trusts twice.
			if (Scripts[closing].Modified) {
				SaveScriptTab(Scripts[closing]);
			}
			CloseScriptTab(closing);
		}
	}
}
