#include <engine/world/DataStore.hpp>

#include <array>
#include <utility>

namespace engine::world {
	namespace {
		DataStoreStatus FromFileStatus(const SharedStoreFileStatus status) {
			switch (status) {
			case SharedStoreFileStatus::Ok:
				return DataStoreStatus::Ok;
			case SharedStoreFileStatus::NotFound:
				return DataStoreStatus::NotFound;
			case SharedStoreFileStatus::IoError:
				return DataStoreStatus::IoError;
			case SharedStoreFileStatus::Malformed:
			case SharedStoreFileStatus::WrongStore:
				return DataStoreStatus::Malformed;
			}
			return DataStoreStatus::Malformed;
		}

		bool ValidRouteName(const core::Name name) {
			return name.IsValid() && !name.Text().empty() &&
				   name.Text().size() <= MAXIMUM_DATASTORE_NAME_BYTES;
		}

		std::string EncodedName(const std::string_view name) {
			constexpr std::array<char, 16> HEX{
				'0',
				'1',
				'2',
				'3',
				'4',
				'5',
				'6',
				'7',
				'8',
				'9',
				'a',
				'b',
				'c',
				'd',
				'e',
				'f',
			};
			std::string encoded(name.size() * 2, '0');
			for (size_t index = 0; index < name.size(); index++) {
				const auto byte = static_cast<unsigned char>(name[index]);
				encoded[index * 2] = HEX[byte >> 4u];
				encoded[index * 2 + 1] = HEX[byte & 0x0fu];
			}
			return encoded;
		}

		class FileDataStoreAdapter final : public DataStoreAdapter {
		  public:
			FileDataStoreAdapter(std::filesystem::path root, const SharedStoreEnvironment environment)
				: Root(std::move(root)), Environment(environment) {}

			DataStoreStatus Load(
				const core::Name store, std::vector<SharedStoreEntry> &entries, std::string &error
			) override {
				if (!ValidRouteName(store)) {
					error = "invalid datastore name";
					return DataStoreStatus::Refused;
				}
				return FromFileStatus(LoadSharedStoreFile(
					DataStoreFilePath(Root, Environment, store), BusKind::DataStore, entries, error
				));
			}

			DataStoreStatus Save(
				const core::Name store, const std::span<const SharedStoreEntry> entries, std::string &error
			) override {
				if (!ValidRouteName(store)) {
					error = "invalid datastore name";
					return DataStoreStatus::Refused;
				}
				return FromFileStatus(SaveSharedStoreFile(
					DataStoreFilePath(Root, Environment, store), BusKind::DataStore, entries, error
				));
			}

		  private:
			std::filesystem::path Root;
			SharedStoreEnvironment Environment;
		};
	}

	const char *Describe(const DataStoreStatus status) {
		switch (status) {
		case DataStoreStatus::Ok:
			return "ok";
		case DataStoreStatus::NotFound:
			return "not found";
		case DataStoreStatus::NoRoute:
			return "no route";
		case DataStoreStatus::AdapterUnavailable:
			return "adapter unavailable";
		case DataStoreStatus::IoError:
			return "I/O error";
		case DataStoreStatus::Malformed:
			return "malformed";
		case DataStoreStatus::Refused:
			return "refused";
		}
		return "unknown";
	}

	bool DataStorageServices::Add(const core::Name name, std::unique_ptr<DataStoreAdapter> adapter) {
		if (!ValidRouteName(name) || adapter == nullptr) {
			return false;
		}
		return Entries.emplace(std::string(name.Text()), std::move(adapter)).second;
	}

	DataStoreAdapter *DataStorageServices::Find(const core::Name name) {
		const auto found = Entries.find(name.Text());
		return found == Entries.end() ? nullptr : found->second.get();
	}

	const DataStoreAdapter *DataStorageServices::Find(const core::Name name) const {
		const auto found = Entries.find(name.Text());
		return found == Entries.end() ? nullptr : found->second.get();
	}

	bool DataStoreRouter::AddAdapter(const core::Name name, std::unique_ptr<DataStoreAdapter> adapter) {
		return Services.Add(name, std::move(adapter));
	}

	bool DataStoreRouter::Assign(const core::Name store, const core::Name adapter) {
		if (!ValidRouteName(store) || !ValidRouteName(adapter) || Services.Find(adapter) == nullptr) {
			return false;
		}
		Routes.insert_or_assign(std::string(store.Text()), std::string(adapter.Text()));
		return true;
	}

	bool DataStoreRouter::Unassign(const core::Name store) {
		if (!ValidRouteName(store)) {
			return false;
		}
		const auto found = Routes.find(store.Text());
		if (found == Routes.end()) {
			return false;
		}
		Routes.erase(found);
		return true;
	}

	core::Name DataStoreRouter::AdapterFor(const core::Name store) const {
		const auto found = Routes.find(store.Text());
		return found == Routes.end() ? core::Name{} : core::Name(found->second);
	}

	DataStoreStatus DataStoreRouter::Load(
		const core::Name store, std::vector<SharedStoreEntry> &entries, std::string &error
	) {
		error.clear();
		const core::Name adapterName = AdapterFor(store);
		if (!adapterName.IsValid()) {
			error = "no adapter assigned to datastore '" + std::string(store.Text()) + "'";
			return DataStoreStatus::NoRoute;
		}
		DataStoreAdapter *adapter = Services.Find(adapterName);
		if (adapter == nullptr) {
			error = "assigned datastore adapter is unavailable";
			return DataStoreStatus::AdapterUnavailable;
		}
		return adapter->Load(store, entries, error);
	}

	DataStoreStatus DataStoreRouter::Save(
		const core::Name store, const std::span<const SharedStoreEntry> entries, std::string &error
	) {
		error.clear();
		const core::Name adapterName = AdapterFor(store);
		if (!adapterName.IsValid()) {
			error = "no adapter assigned to datastore '" + std::string(store.Text()) + "'";
			return DataStoreStatus::NoRoute;
		}
		DataStoreAdapter *adapter = Services.Find(adapterName);
		if (adapter == nullptr) {
			error = "assigned datastore adapter is unavailable";
			return DataStoreStatus::AdapterUnavailable;
		}
		return adapter->Save(store, entries, error);
	}

	std::filesystem::path DataStoreFilePath(
		const std::filesystem::path &root, const SharedStoreEnvironment environment, const core::Name store
	) {
		const std::filesystem::path environmentRoot = root / Describe(environment);
		if (store.Text() == DEFAULT_DATASTORE) {
			return environmentRoot / "datastore.bin";
		}
		return environmentRoot / "datastores" / (EncodedName(store.Text()) + ".bin");
	}

	std::unique_ptr<DataStoreAdapter>
	MakeFileDataStoreAdapter(std::filesystem::path root, const SharedStoreEnvironment environment) {
		return std::make_unique<FileDataStoreAdapter>(std::move(root), environment);
	}
}
