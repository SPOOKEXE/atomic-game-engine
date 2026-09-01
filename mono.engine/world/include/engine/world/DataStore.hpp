#pragma once

// Host-side persistence routing for DataStore snapshots.
//
// The ECS-facing bus remains the authoritative live store. A host calls this
// boundary outside world ticks to load or save copied entries through a named
// adapter. Remote providers can therefore arrive without putting network state
// in `BusRouter` or changing the script service.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/world/SharedStoreFile.hpp>
#include <engine/world/SharedStores.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::world {
	// The conventional route used by the current DataStore service.
	inline constexpr std::string_view DEFAULT_DATASTORE = "default";
	// The conventional name for the built-in local file adapter.
	inline constexpr std::string_view FILE_DATASTORE_ADAPTER = "file";

	// Names are encoded into local filenames and remote routes.
	inline constexpr size_t MAXIMUM_DATASTORE_NAME_BYTES = 96;

	// Provider-independent result of loading or saving a datastore.
	enum class DataStoreStatus {
		// The requested operation completed.
		Ok,
		// No persisted image exists for the datastore.
		NotFound,
		// The logical datastore has no adapter assignment.
		NoRoute,
		// The assigned adapter is no longer registered.
		AdapterUnavailable,
		// The adapter could not read or write its backing storage.
		IoError,
		// The persisted snapshot failed structural validation.
		Malformed,
		// The adapter rejected the request before accessing storage.
		Refused,
	};

	// Returns a stable diagnostic spelling for an adapter result.
	const char *Describe(DataStoreStatus status);

	// One persistence backend behind the datastore router.
	class DataStoreAdapter {
	  public:
		virtual ~DataStoreAdapter() = default;

		// Loads one named datastore without changing `entries` on failure.
		[[nodiscard]] virtual DataStoreStatus
		Load(core::Name store, std::vector<SharedStoreEntry> &entries, std::string &error) = 0;

		// Saves one complete named datastore snapshot.
		[[nodiscard]] virtual DataStoreStatus
		Save(core::Name store, std::span<const SharedStoreEntry> entries, std::string &error) = 0;
	};

	// Owns the named adapters available to one host.
	class DataStorageServices {
	  public:
		// Registers a provider name once.
		[[nodiscard]] bool Add(core::Name name, std::unique_ptr<DataStoreAdapter> adapter);

		// Finds a mutable provider, or null when it is not registered.
		DataStoreAdapter *Find(core::Name name);

		// Finds a provider, or null when it is not registered.
		const DataStoreAdapter *Find(core::Name name) const;

	  private:
		std::map<std::string, std::unique_ptr<DataStoreAdapter>, std::less<>> Entries;
	};

	// Assigns logical datastores to persistence adapters.
	class DataStoreRouter {
	  public:
		// Registers one adapter under a stable name.
		[[nodiscard]] bool AddAdapter(core::Name name, std::unique_ptr<DataStoreAdapter> adapter);

		// Assigns or reassigns a datastore to an already registered adapter.
		[[nodiscard]] bool Assign(core::Name store, core::Name adapter);

		// Removes a datastore route.
		[[nodiscard]] bool Unassign(core::Name store);

		// Returns the adapter assigned to a datastore, or an invalid name.
		core::Name AdapterFor(core::Name store) const;

		// Loads through the assigned adapter.
		[[nodiscard]] DataStoreStatus
		Load(core::Name store, std::vector<SharedStoreEntry> &entries, std::string &error);

		// Saves through the assigned adapter.
		[[nodiscard]] DataStoreStatus
		Save(core::Name store, std::span<const SharedStoreEntry> entries, std::string &error);

	  private:
		DataStorageServices Services;
		std::map<std::string, std::string, std::less<>> Routes;
	};

	// Resolves a local file for one logical datastore. The default route keeps
	// the existing `<root>/mock|live/datastore.bin` path.
	std::filesystem::path DataStoreFilePath(
		const std::filesystem::path &root, SharedStoreEnvironment environment, core::Name store
	);

	// Builds the local atomic-file adapter.
	std::unique_ptr<DataStoreAdapter>
	MakeFileDataStoreAdapter(std::filesystem::path root, SharedStoreEnvironment environment);
}
