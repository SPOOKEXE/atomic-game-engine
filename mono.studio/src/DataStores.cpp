#include <engine/core/Log.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <imgui.h>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {
	std::filesystem::path Editor::DataStoreRoot() const {
		return Prefs.DataStoreRoot.empty() ? ConfigPath("stores")
										   : std::filesystem::path(Prefs.DataStoreRoot);
	}

	bool Editor::ConfigureDataStore(const bool flushActive) {
		if (flushActive && DataStoreReady && !FlushDataStore()) {
			return false;
		}

		DataStoreReady = false;
		ActiveDataStorePath.clear();
		SavedDataStoreEntries.clear();
		DataStoreError.clear();

		if (!Prefs.DataStoreEnabled || Universe == nullptr) {
			return true;
		}

		ActiveDataStorePath = engine::world::SharedStorePath(
			DataStoreRoot(), Prefs.DataStoreEnvironment, engine::world::BusKind::DataStore
		);

		std::vector<engine::world::SharedStoreEntry> loaded;
		std::string error;
		const engine::world::SharedStoreFileStatus status = engine::world::LoadSharedStoreFile(
			ActiveDataStorePath, engine::world::BusKind::DataStore, loaded, error
		);
		if (status != engine::world::SharedStoreFileStatus::Ok &&
			status != engine::world::SharedStoreFileStatus::NotFound) {
			DataStoreError = error.empty() ? engine::world::Describe(status) : std::move(error);
			ENGINE_ERROR("studio datastore: {}", DataStoreError);
			return false;
		}

		if (status == engine::world::SharedStoreFileStatus::Ok) {
			if (Universe->ReplaceSharedStoreEntries(engine::world::BusKind::DataStore, loaded) !=
				engine::world::BusStatus::Ok) {
				DataStoreError = "the decoded image was refused by the universe";
				ENGINE_ERROR("studio datastore: {}", DataStoreError);
				return false;
			}
			SavedDataStoreEntries = std::move(loaded);
		} else {
			SavedDataStoreEntries = Universe->SharedStoreEntries(engine::world::BusKind::DataStore);
		}

		DataStoreReady = true;
		NextDataStoreFlush = engine::core::Clock::Seconds() + 1.0;
		ENGINE_INFO("studio datastore: {}", ActiveDataStorePath.string());
		return true;
	}

	bool Editor::FlushDataStore() {
		if (!DataStoreReady || Universe == nullptr || ActiveDataStorePath.empty()) {
			return true;
		}

		std::vector<engine::world::SharedStoreEntry> current =
			Universe->SharedStoreEntries(engine::world::BusKind::DataStore);
		if (current == SavedDataStoreEntries) {
			return true;
		}

		std::string error;
		const engine::world::SharedStoreFileStatus status = engine::world::SaveSharedStoreFile(
			ActiveDataStorePath, engine::world::BusKind::DataStore, current, error
		);
		if (status != engine::world::SharedStoreFileStatus::Ok) {
			DataStoreError = error.empty() ? engine::world::Describe(status) : std::move(error);
			ENGINE_ERROR("studio datastore: {}", DataStoreError);
			return false;
		}

		SavedDataStoreEntries = std::move(current);
		DataStoreError.clear();
		return true;
	}

	void Editor::DrawDataStoreSettings() {
		ImGui::SeparatorText("Local provider");

		if (ImGui::Checkbox("Enable durable DataStore", &Prefs.DataStoreEnabled)) {
			(void)ConfigureDataStore();
		}
		ImGui::TextDisabled("MemoryStore remains ephemeral and is cleared when Studio exits.");

		ImGui::BeginDisabled(!Prefs.DataStoreEnabled);
		const std::string defaultRoot = ConfigPath("stores").string();
		TextField("Root folder", Prefs.DataStoreRoot, defaultRoot.c_str());
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			(void)ConfigureDataStore();
		}

		const char *environments[] = {"Mock", "Live"};
		int environment = Prefs.DataStoreEnvironment == engine::world::SharedStoreEnvironment::Mock ? 0 : 1;
		if (ImGui::Combo("Environment", &environment, environments, IM_ARRAYSIZE(environments))) {
			Prefs.DataStoreEnvironment = environment == 0 ? engine::world::SharedStoreEnvironment::Mock
														  : engine::world::SharedStoreEnvironment::Live;
			(void)ConfigureDataStore();
		}

		if (ImGui::Button("Reload from disk")) {
			(void)ConfigureDataStore(false);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save now")) {
			(void)FlushDataStore();
		}

		const std::filesystem::path shown =
			ActiveDataStorePath.empty()
				? engine::world::SharedStorePath(
					  DataStoreRoot(), Prefs.DataStoreEnvironment, engine::world::BusKind::DataStore
				  )
				: ActiveDataStorePath;
		ImGui::TextWrapped("%s", shown.string().c_str());
		if (!DataStoreError.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", DataStoreError.c_str());
		} else if (DataStoreReady) {
			ImGui::TextDisabled("ready");
		}
		ImGui::EndDisabled();
	}
}
