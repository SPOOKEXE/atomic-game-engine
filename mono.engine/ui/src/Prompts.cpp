#include <engine/ui/Browse.hpp>
#include <engine/ui/Fields.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/PerCallSite.hpp>
#include <engine/ui/Prompts.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <filesystem>
#include <imgui.h>

namespace engine::ui {

	bool FilePrompt(
		const char *title,
		std::string &path,
		const char *accept,
		const std::vector<std::string> &extensions,
		bool mustExist
	) {
		bool confirmed = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(Scaled(640.0f), Scaled(460.0f)), ImGuiCond_Appearing);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return false;
		}

		// **Where each dialog is looking, kept per title.** Six dialogs share
		// this function and they are not looking at the same place: Open starts
		// where the game is, Export starts wherever the last export went. One
		// shared directory would make each of them jump to whichever was used
		// last.
		//
		// `PerCallSite` is that pattern, written once - this was the third
		// transcription of it and the comment here used to say so.
		struct Browsing {
			std::filesystem::path Where;
			std::string Name;
		};

		Browsing *const state = &PerCallSite<Browsing>(title);

		// Opened fresh: start from whatever path the caller had, which is the
		// game's own folder far more often than not.
		if (ImGui::IsWindowAppearing()) {
			const std::filesystem::path given(path);
			state->Where = given;
			state->Name = given.has_filename() ? given.filename().string() : std::string();
		}

		const Listing listing = BrowseDirectory(state->Where, extensions);
		state->Where = listing.Directory;

		// The path bar. Not editable - the field at the bottom is where a path
		// is typed, and two places to type one would be two places for them to
		// disagree.
		ImGui::TextDisabled("%s", listing.Directory.string().c_str());
		ImGui::Separator();

		const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (!listing.Error.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, WarningColour());
				ImGui::TextWrapped("%s", listing.Error.c_str());
				ImGui::PopStyleColor();
			}

			if (!listing.Parent.empty()) {
				if (ImGui::Selectable("..", false, ImGuiSelectableFlags_AllowDoubleClick)) {
					state->Where = listing.Parent;
				}
			}

			for (const BrowseEntry &entry : listing.Entries) {
				// The id is the path rather than the name, so two folders with
				// the same name in different places are two rows.
				ImGui::PushID(entry.Path.string().c_str());

				const bool selected = !entry.Directory && entry.Name == state->Name;

				if (entry.Directory) {
					ImGui::PushStyleColor(ImGuiCol_Text, AccentColour());
				}

				if (ImGui::Selectable(
						(entry.Directory ? entry.Name + "/" : entry.Name).c_str(),
						selected,
						ImGuiSelectableFlags_AllowDoubleClick
					)) {
					if (entry.Directory) {
						// Single click descends. A folder is not a thing this
						// dialog can return, so there is nothing else a click
						// on one could mean.
						state->Where = entry.Path;
						state->Name.clear();
					} else {
						state->Name = entry.Name;

						// Double-click is confirm, which is what every file
						// dialog does and what a person tries first.
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
							confirmed = true;
						}
					}
				}

				if (entry.Directory) {
					ImGui::PopStyleColor();
				}

				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::SetNextItemWidth(-1.0f);
		const bool entered = TextField("##name", state->Name, "file name");

		const std::filesystem::path chosen =
			state->Name.empty() ? std::filesystem::path{} : listing.Directory / state->Name;

		std::error_code code;
		const bool exists = !chosen.empty() && std::filesystem::exists(chosen, code);

		// **Refused rather than allowed to fail later.** Open on a path that is
		// not there used to be discovered by pressing the button and reading a
		// log line; a disabled button with the reason beside it is the same
		// information before the click rather than after.
		const bool usable = !chosen.empty() && (!mustExist || exists);

		const ImVec2 button(Scaled(120.0f), 0.0f);

		ImGui::BeginDisabled(!usable);
		if (ImGui::Button(accept, button) || (entered && usable)) {
			confirmed = true;
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		if (!chosen.empty() && mustExist && !exists) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, MutedColour());
			ImGui::TextUnformatted("no such file");
			ImGui::PopStyleColor();
		}

		if (confirmed) {
			path = chosen.string();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

	bool FolderPrompt(const char *title, std::string &path, const char *accept) {
		bool confirmed = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(Scaled(640.0f), Scaled(460.0f)), ImGuiCond_Appearing);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return false;
		}

		// Where this dialog is looking, kept per title - `FilePrompt`'s reason
		// and its pattern.
		std::filesystem::path *const where = &PerCallSite<std::filesystem::path>(title);

		if (ImGui::IsWindowAppearing()) {
			*where = std::filesystem::path(path);
		}

		// **No extension filter, and the files are listed anyway.** A folder
		// browser that showed only directories makes an empty folder and the
		// wrong folder look identical, which is exactly the mistake somebody is
		// about to make when they import a hundred files from the wrong place.
		const Listing listing = BrowseDirectory(*where, {});
		*where = listing.Directory;

		ImGui::TextDisabled("%s", listing.Directory.string().c_str());
		ImGui::Separator();

		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (!listing.Error.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, WarningColour());
				ImGui::TextWrapped("%s", listing.Error.c_str());
				ImGui::PopStyleColor();
			}

			if (!listing.Parent.empty()) {
				if (ImGui::Selectable("..")) {
					*where = listing.Parent;
				}
			}

			size_t files = 0;
			for (const BrowseEntry &entry : listing.Entries) {
				ImGui::PushID(entry.Path.string().c_str());

				if (entry.Directory) {
					ImGui::PushStyleColor(ImGuiCol_Text, AccentColour());
					if (ImGui::Selectable((entry.Name + "/").c_str())) {
						*where = entry.Path;
					}
					ImGui::PopStyleColor();
				} else {
					// Shown and not selectable: this dialog returns the folder,
					// and a row that highlighted but did nothing would be worse
					// than one that plainly cannot be picked.
					ImGui::TextDisabled("%s", entry.Name.c_str());
					files++;
				}

				ImGui::PopID();
			}

			if (files == 0 && listing.Entries.empty()) {
				ImGui::TextDisabled("(empty)");
			}
		}
		ImGui::EndChild();

		const ImVec2 button(Scaled(140.0f), 0.0f);

		if (ImGui::Button(accept, button)) {
			confirmed = true;
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		if (confirmed) {
			path = listing.Directory.string();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}
	bool FoldersPrompt(
		const char *title, std::string &path, std::vector<std::string> &chosen, const char *accept
	) {
		bool confirmed = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(Scaled(640.0f), Scaled(460.0f)), ImGuiCond_Appearing);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return false;
		}

		// Where this dialog is looking and what it has ticked, kept per title -
		// `FilePrompt`'s reason and its pattern. The ticks are held as absolute
		// paths rather than names, because two folders in different parents can
		// share a name and a set of names could not tell them apart.
		struct Picking {
			std::filesystem::path Where;
			std::vector<std::filesystem::path> Ticked;
		};

		Picking *const state = &PerCallSite<Picking>(title);

		if (ImGui::IsWindowAppearing()) {
			state->Where = std::filesystem::path(path);
			state->Ticked.clear();
		}

		const Listing listing = BrowseDirectory(state->Where, {});
		state->Where = listing.Directory;
		path = listing.Directory.string();

		ImGui::TextDisabled("%s", listing.Directory.string().c_str());
		ImGui::Separator();

		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (!listing.Error.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, WarningColour());
				ImGui::TextWrapped("%s", listing.Error.c_str());
				ImGui::PopStyleColor();
			}

			if (!listing.Parent.empty()) {
				// No tick box on `..`: the parent is a place to go rather than
				// a thing to pick, and one that could be ticked would let
				// somebody select a directory they are not looking at.
				ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0f));
				ImGui::SameLine();
				if (ImGui::Selectable("..")) {
					state->Where = listing.Parent;
				}
			}

			size_t files = 0;
			for (const BrowseEntry &entry : listing.Entries) {
				ImGui::PushID(entry.Path.string().c_str());

				if (!entry.Directory) {
					// Shown and greyed, for `FolderPrompt`'s reason: hiding
					// them makes an empty folder and the wrong folder look
					// identical.
					ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0f));
					ImGui::SameLine();
					ImGui::TextDisabled("%s", entry.Name.c_str());
					files++;
					ImGui::PopID();
					continue;
				}

				const auto found = std::find(state->Ticked.begin(), state->Ticked.end(), entry.Path);
				bool ticked = found != state->Ticked.end();

				if (ImGui::Checkbox("##tick", &ticked)) {
					if (ticked) {
						state->Ticked.push_back(entry.Path);
					} else {
						state->Ticked.erase(found);
					}
				}
				ImGui::SameLine();

				ImGui::PushStyleColor(ImGuiCol_Text, AccentColour());
				if (ImGui::Selectable((entry.Name + "/").c_str())) {
					state->Where = entry.Path;
				}
				ImGui::PopStyleColor();

				ImGui::PopID();
			}

			if (files == 0 && listing.Entries.empty()) {
				ImGui::TextDisabled("(empty)");
			}
		}
		ImGui::EndChild();

		// **The count is on the button.** A folder ticked three directories ago
		// is otherwise invisible from here, and a person who cannot see what
		// they have selected cannot tell a confirm from a mistake.
		const std::string caption = state->Ticked.empty()
										? std::string(accept)
										: std::string(accept) + " " + std::to_string(state->Ticked.size());

		const ImVec2 button(Scaled(160.0f), 0.0f);

		ImGui::BeginDisabled(state->Ticked.empty());
		if (ImGui::Button(caption.c_str(), button)) {
			chosen.clear();
			for (const std::filesystem::path &picked : state->Ticked) {
				chosen.push_back(picked.string());
			}
			confirmed = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

}
