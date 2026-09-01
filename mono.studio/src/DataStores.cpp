#include <engine/core/Log.hpp>
#include <engine/datastore/Backend.hpp>
#include <engine/datastore/Http.hpp>
#include <engine/datastore/Sqlite.hpp>
#include <engine/ui/Prompts.hpp>
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
		ActiveDataStoreLocation.clear();
		SavedDataStoreEntries.clear();
		DataStoreError.clear();

		if (!Prefs.DataStoreEnabled || Universe == nullptr) {
			return true;
		}

		const bool remote = Prefs.DataStoreProvider == engine::datastore::Provider::Http;
		const bool sqlite = !remote && Prefs.DataStoreBackend == engine::datastore::Backend::SQLite;
		const engine::core::Name adapterName(
			remote ? engine::datastore::HTTP_DATASTORE_ADAPTER
				   : (sqlite ? engine::datastore::SQLITE_DATASTORE_ADAPTER
							 : engine::world::FILE_DATASTORE_ADAPTER)
		);
		const engine::core::Name storeName(engine::world::DEFAULT_DATASTORE);
		auto persistence = std::make_unique<engine::world::DataStoreRouter>();
		std::unique_ptr<engine::world::DataStoreAdapter> adapter;
		if (remote) {
			const std::optional<engine::net::Endpoint> endpoint =
				engine::net::Endpoint::Parse(Prefs.DataStoreHttpEndpoint);
			if (!endpoint) {
				DataStoreError = "HTTP endpoint must be a numeric HOST:PORT";
				return false;
			}
			engine::datastore::HttpDataStoreSettings settings{
				.Server = *endpoint,
				.Host = Prefs.DataStoreHttpHost,
				.TargetPrefix = Prefs.DataStoreHttpPrefix,
				.Authorization = Prefs.DataStoreHttpAuthorization,
			};
			adapter = engine::datastore::MakeHttpDataStoreAdapter(std::move(settings));
			ActiveDataStoreLocation = "http://" + endpoint->Text() + Prefs.DataStoreHttpPrefix;
		} else if (sqlite) {
			adapter =
				engine::datastore::MakeSqliteDataStoreAdapter(DataStoreRoot(), Prefs.DataStoreEnvironment);
			ActiveDataStoreLocation =
				engine::datastore::SqliteDataStorePath(DataStoreRoot(), Prefs.DataStoreEnvironment).string();
		} else {
			adapter = engine::world::MakeFileDataStoreAdapter(DataStoreRoot(), Prefs.DataStoreEnvironment);
			ActiveDataStoreLocation =
				engine::world::DataStoreFilePath(DataStoreRoot(), Prefs.DataStoreEnvironment, storeName)
					.string();
		}
		if (!persistence->AddAdapter(adapterName, std::move(adapter)) ||
			!persistence->Assign(storeName, adapterName)) {
			DataStoreError = "could not configure the datastore persistence route";
			ENGINE_ERROR("studio datastore: {}", DataStoreError);
			return false;
		}

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
		ENGINE_INFO("studio datastore: {}", ActiveDataStoreLocation);
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

	void Editor::DrawDataStores() {
		if (!ShowDataStores) {
			return;
		}
		if (!ImGui::Begin("DataStores", &ShowDataStores)) {
			ImGui::End();
			return;
		}
		DrawDataStoreSettings();
		ImGui::End();
	}

	void Editor::DrawDataStoreSettings() {
		ImGui::SeparatorText("DataStore provider");

		if (ImGui::Checkbox("Enable durable DataStore", &Prefs.DataStoreEnabled)) {
			(void)ConfigureDataStore();
		}
		ImGui::TextDisabled("MemoryStore remains ephemeral and is cleared when Studio exits.");

		ImGui::BeginDisabled(!Prefs.DataStoreEnabled);
		const char *providers[] = {"File", "HTTP"};
		int provider = Prefs.DataStoreProvider == engine::datastore::Provider::File ? 0 : 1;
		if (ImGui::Combo("Provider", &provider, providers, IM_ARRAYSIZE(providers))) {
			Prefs.DataStoreProvider =
				provider == 0 ? engine::datastore::Provider::File : engine::datastore::Provider::Http;
			(void)ConfigureDataStore();
		}

		if (Prefs.DataStoreProvider == engine::datastore::Provider::File) {
			const char *backends[] = {"Binary (.bin)", "SQLite (.sqlite3)"};
			int backend = Prefs.DataStoreBackend == engine::datastore::Backend::Binary ? 0 : 1;
			if (ImGui::Combo("Backend", &backend, backends, IM_ARRAYSIZE(backends))) {
				Prefs.DataStoreBackend =
					backend == 0 ? engine::datastore::Backend::Binary : engine::datastore::Backend::SQLite;
				(void)ConfigureDataStore();
			}

			const std::string root = DataStoreRoot().string();
			ImGui::TextUnformatted("Root folder");
			ImGui::TextWrapped("%s", root.c_str());
			if (ImGui::Button("Select folder...")) {
				DataStoreBrowsePath = root;
				ImGui::OpenPopup("Select DataStore Root");
			}
			if (engine::ui::FolderPrompt("Select DataStore Root", DataStoreBrowsePath, "Use folder")) {
				Prefs.DataStoreRoot = DataStoreBrowsePath;
				(void)ConfigureDataStore();
			}

			const char *environments[] = {"Mock", "Live"};
			int environment =
				Prefs.DataStoreEnvironment == engine::world::SharedStoreEnvironment::Mock ? 0 : 1;
			if (ImGui::Combo("Environment", &environment, environments, IM_ARRAYSIZE(environments))) {
				Prefs.DataStoreEnvironment = environment == 0 ? engine::world::SharedStoreEnvironment::Mock
															  : engine::world::SharedStoreEnvironment::Live;
				(void)ConfigureDataStore();
			}
		} else {
			TextField("Endpoint", Prefs.DataStoreHttpEndpoint, "127.0.0.1:8080");
			bool connectionEdited = ImGui::IsItemDeactivatedAfterEdit();
			TextField("Host header", Prefs.DataStoreHttpHost, "localhost");
			connectionEdited = ImGui::IsItemDeactivatedAfterEdit() || connectionEdited;
			TextField("Target prefix", Prefs.DataStoreHttpPrefix, "/datastores/");
			connectionEdited = ImGui::IsItemDeactivatedAfterEdit() || connectionEdited;
			TextField("Authorization", Prefs.DataStoreHttpAuthorization, "Bearer ...");
			connectionEdited = ImGui::IsItemDeactivatedAfterEdit() || connectionEdited;
			if (connectionEdited) {
				(void)ConfigureDataStore();
			}
		}

		if (ImGui::Button("Reload from provider")) {
			(void)ConfigureDataStore(false);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save now")) {
			(void)FlushDataStore();
		}

		std::string inactiveLocation;
		if (Prefs.DataStoreProvider == engine::datastore::Provider::Http) {
			inactiveLocation = "http://" + Prefs.DataStoreHttpEndpoint + Prefs.DataStoreHttpPrefix;
		} else if (Prefs.DataStoreBackend == engine::datastore::Backend::SQLite) {
			inactiveLocation =
				engine::datastore::SqliteDataStorePath(DataStoreRoot(), Prefs.DataStoreEnvironment).string();
		} else {
			inactiveLocation = engine::world::DataStoreFilePath(
								   DataStoreRoot(),
								   Prefs.DataStoreEnvironment,
								   engine::core::Name(engine::world::DEFAULT_DATASTORE)
			)
								   .string();
		}
		const std::string shown =
			ActiveDataStoreLocation.empty() ? inactiveLocation : ActiveDataStoreLocation;
		ImGui::TextWrapped("%s", shown.c_str());
		if (!DataStoreError.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", DataStoreError.c_str());
		} else if (DataStoreReady) {
			ImGui::TextDisabled("ready");
		}
		ImGui::EndDisabled();
	}
}
