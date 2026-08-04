#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/ui/Theme.hpp>

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

						if (Mode != RunMode::Edit) {
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

						ImGui::Separator();

						// Monospace would be better and imgui's default font is
						// not one. Loading a font is a file this repository does
						// not have and a licence somebody has to choose, so the
						// default is used and the gap is named here rather than
						// left to be noticed.
						if (CodeField("##text", tab.Text, -1.0f, -1.0f)) {
							tab.Modified = true;
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
