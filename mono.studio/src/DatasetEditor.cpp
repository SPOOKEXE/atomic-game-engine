#include "DatasetValue.hpp"

#include <engine/core/Name.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {
	void Editor::DrawDatasets() {
		if (!ShowDatasets || Universe == nullptr) {
			return;
		}
		if (!ImGui::Begin("Dataset Editor", &ShowDatasets)) {
			ImGui::End();
			return;
		}

		const char *stores[] = {"DataStore", "MemoryStore"};
		int storeIndex = DatasetStore == engine::world::BusKind::DataStore ? 0 : 1;
		ImGui::SetNextItemWidth(engine::ui::Scaled(150.0f));
		if (ImGui::Combo("##dataset-store", &storeIndex, stores, IM_ARRAYSIZE(stores))) {
			DatasetStore =
				storeIndex == 0 ? engine::world::BusKind::DataStore : engine::world::BusKind::MemoryStore;
			DatasetSelectedKey.clear();
			DatasetCreating = false;
			DatasetEditError.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("New")) {
			DatasetCreating = true;
			DatasetSelectedKey.clear();
			DatasetKeyDraft.clear();
			DatasetValueDraft = "{\n  \"type\": \"string\",\n  \"value\": {\n    \"encoding\": \"utf8\",\n   "
								" \"value\": \"\"\n  }\n}";
			DatasetEditError.clear();
		}
		ImGui::SameLine();
		ImGui::TextDisabled(DatasetStore == engine::world::BusKind::DataStore ? "durable" : "ephemeral");

		std::vector<engine::world::SharedStoreEntry> entries = Universe->SharedStoreEntries(DatasetStore);
		const float keyWidth = engine::ui::Scaled(230.0f);
		if (ImGui::BeginChild("##dataset-keys", ImVec2(keyWidth, 0.0f), ImGuiChildFlags_Borders)) {
			ImGui::SetNextItemWidth(-1.0f);
			TextField("##dataset-filter", DatasetFilter, "filter keys");
			ImGui::Separator();
			for (const engine::world::SharedStoreEntry &entry : entries) {
				const std::string key(entry.Key.Text());
				if (!DatasetFilter.empty() && key.find(DatasetFilter) == std::string::npos) {
					continue;
				}
				if (ImGui::Selectable(key.c_str(), !DatasetCreating && DatasetSelectedKey == key)) {
					DatasetCreating = false;
					DatasetSelectedKey = key;
					DatasetKeyDraft = key;
					DatasetEditError.clear();
					if (!DatasetValueToText(entry.Value, DatasetValueDraft, DatasetEditError)) {
						DatasetValueDraft.clear();
					}
				}
			}
		}
		ImGui::EndChild();
		ImGui::SameLine();

		if (ImGui::BeginChild("##dataset-value", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
			if (!DatasetCreating && DatasetSelectedKey.empty()) {
				ImGui::TextDisabled("Select a key or create one.");
			} else {
				ImGui::BeginDisabled(!DatasetCreating);
				TextField("Key", DatasetKeyDraft, "a stable key name");
				ImGui::EndDisabled();

				const float editorHeight = std::max(
					engine::ui::Scaled(120.0f), ImGui::GetContentRegionAvail().y - engine::ui::Scaled(70.0f)
				);
				CodeField("##dataset-document", DatasetValueDraft, nullptr, -1.0f, editorHeight);

				if (!DatasetEditError.empty()) {
					ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", DatasetEditError.c_str());
				}

				if (ImGui::Button("Save")) {
					DatasetEditError.clear();
					const engine::core::Name key(DatasetKeyDraft);
					if (!key.IsValid()) {
						DatasetEditError = "key cannot be empty";
					} else if (DatasetCreating && std::any_of(
													  entries.begin(),
													  entries.end(),
													  [&](const engine::world::SharedStoreEntry &entry) {
														  return entry.Key == key;
													  }
												  )) {
						DatasetEditError = "that key already exists";
					} else {
						std::vector<std::byte> encoded;
						if (DatasetValueFromText(DatasetValueDraft, encoded, DatasetEditError)) {
							const engine::world::BusStatus status =
								Universe->SetSharedStoreValue(DatasetStore, key, encoded);
							if (status == engine::world::BusStatus::Ok) {
								DatasetCreating = false;
								DatasetSelectedKey = DatasetKeyDraft;
								if (DatasetStore == engine::world::BusKind::DataStore) {
									(void)FlushDataStore();
								}
							} else {
								DatasetEditError = engine::world::Describe(status);
							}
						}
					}
				}

				if (!DatasetCreating) {
					ImGui::SameLine();
					if (ImGui::Button("Delete")) {
						ImGui::OpenPopup("Delete dataset key?");
					}
				}

				if (ImGui::BeginPopupModal(
						"Delete dataset key?", nullptr, ImGuiWindowFlags_AlwaysAutoResize
					)) {
					ImGui::Text("Delete '%s'?", DatasetSelectedKey.c_str());
					ImGui::TextDisabled("Scripts reading it will receive no value.");
					if (ImGui::Button("Delete")) {
						const engine::world::BusStatus status = Universe->RemoveSharedStoreValue(
							DatasetStore, engine::core::Name(DatasetSelectedKey)
						);
						if (status == engine::world::BusStatus::Ok) {
							DatasetSelectedKey.clear();
							DatasetKeyDraft.clear();
							DatasetValueDraft.clear();
							DatasetEditError.clear();
							if (DatasetStore == engine::world::BusKind::DataStore) {
								(void)FlushDataStore();
							}
							ImGui::CloseCurrentPopup();
						} else {
							DatasetEditError = engine::world::Describe(status);
						}
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel")) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			}
		}
		ImGui::EndChild();
		ImGui::End();
	}
}
