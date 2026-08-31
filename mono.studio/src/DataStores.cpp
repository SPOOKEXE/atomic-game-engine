#include <engine/core/Log.hpp>
#include <engine/world/DataStore.hpp>

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
		DataStorePersistence.reset();
		ActiveDataStorePath.clear();
		SavedDataStoreEntries.clear();
		DataStoreError.clear();

		if (!Prefs.DataStoreEnabled || Universe == nullptr) {
			return true;
		}

		const engine::core::Name adapterName(engine::world::FILE_DATASTORE_ADAPTER);
		const engine::core::Name storeName(engine::world::DEFAULT_DATASTORE);
		auto persistence = std::make_unique<engine::world::DataStoreRouter>();
		if (!persistence->AddAdapter(
				adapterName,
				engine::world::MakeFileDataStoreAdapter(DataStoreRoot(), Prefs.DataStoreEnvironment)
			) ||
			!persistence->Assign(storeName, adapterName)) {
			DataStoreError = "could not configure the datastore persistence route";
			ENGINE_ERROR("studio datastore: {}", DataStoreError);
			return false;
		}

		ActiveDataStorePath =
			engine::world::DataStoreFilePath(DataStoreRoot(), Prefs.DataStoreEnvironment, storeName);

		std::vector<engine::world::SharedStoreEntry> loaded;
		std::string error;
		const engine::world::DataStoreStatus status = persistence->Load(storeName, loaded, error);
		if (status != engine::world::DataStoreStatus::Ok &&
			status != engine::world::DataStoreStatus::NotFound) {
			DataStoreError = error.empty() ? engine::world::Describe(status) : std::move(error);
			ENGINE_ERROR("studio datastore: {}", DataStoreError);
			return false;
		}

		if (status == engine::world::DataStoreStatus::Ok) {
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

		DataStorePersistence = std::move(persistence);
		DataStoreReady = true;
		NextDataStoreFlush = engine::core::Clock::Seconds() + 1.0;
		ENGINE_INFO("studio datastore: {}", ActiveDataStorePath.string());
		return true;
	}

	bool Editor::FlushDataStore() {
		if (!DataStoreReady || Universe == nullptr || DataStorePersistence == nullptr) {
			return true;
		}

		std::vector<engine::world::SharedStoreEntry> current =
			Universe->SharedStoreEntries(engine::world::BusKind::DataStore);
		if (current == SavedDataStoreEntries) {
			return true;
		}

		std::string error;
		const engine::world::DataStoreStatus status =
			DataStorePersistence->Save(engine::core::Name(engine::world::DEFAULT_DATASTORE), current, error);
		if (status != engine::world::DataStoreStatus::Ok) {
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

		const std::filesystem::path shown = ActiveDataStorePath.empty()
												? engine::world::DataStoreFilePath(
													  DataStoreRoot(),
													  Prefs.DataStoreEnvironment,
													  engine::core::Name(engine::world::DEFAULT_DATASTORE)
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
