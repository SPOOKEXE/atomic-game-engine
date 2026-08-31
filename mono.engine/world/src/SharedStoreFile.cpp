#include <engine/core/Bytes.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace engine::world {
	namespace {
		constexpr uint64_t MAGIC = 0x4552'4F54'5353'4441ull;
		constexpr uint32_t FORMAT = 1;
		constexpr uint64_t MAXIMUM_FILE_BYTES = 256ull * 1024ull * 1024ull;
		constexpr uint32_t MAXIMUM_ENTRIES = 1'000'000;

		bool Supported(BusKind store) {
			return store == BusKind::MemoryStore || store == BusKind::DataStore;
		}
	}

	const char *Describe(SharedStoreFileStatus status) {
		switch (status) {
		case SharedStoreFileStatus::Ok:
			return "ok";
		case SharedStoreFileStatus::NotFound:
			return "not found";
		case SharedStoreFileStatus::IoError:
			return "I/O error";
		case SharedStoreFileStatus::Malformed:
			return "malformed";
		case SharedStoreFileStatus::WrongStore:
			return "wrong store";
		}
		return "unknown";
	}

	SharedStoreFileStatus SaveSharedStoreFile(
		const std::filesystem::path &path,
		BusKind store,
		std::span<const SharedStoreEntry> entries,
		std::string &error
	) {
		error.clear();
		if (!Supported(store) || entries.size() > MAXIMUM_ENTRIES) {
			error = "unsupported shared store image";
			return SharedStoreFileStatus::WrongStore;
		}

		std::vector<const SharedStoreEntry *> ordered;
		ordered.reserve(entries.size());
		std::unordered_set<uint32_t> keys;
		for (const SharedStoreEntry &entry : entries) {
			if (entry.Store != store || !entry.Key.IsValid() || entry.Value.size() > MAXIMUM_FILE_BYTES ||
				!keys.insert(entry.Key.Id()).second || (store == BusKind::DataStore && entry.Version == 0) ||
				(store == BusKind::MemoryStore && entry.Version != 0)) {
				error = "invalid or duplicate shared store entry";
				return SharedStoreFileStatus::Malformed;
			}
			ordered.push_back(&entry);
		}
		std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
			return left->Key.Text() < right->Key.Text();
		});

		core::ByteWriter writer;
		writer.WriteUInt64(MAGIC);
		writer.WriteUInt32(FORMAT);
		writer.WriteUInt8(static_cast<uint8_t>(store));
		writer.WriteUInt32(static_cast<uint32_t>(ordered.size()));
		for (const SharedStoreEntry *entry : ordered) {
			writer.WriteName(entry->Key);
			writer.WriteUInt64(entry->Version);
			writer.WriteUInt32(static_cast<uint32_t>(entry->Value.size()));
			writer.WriteRaw(entry->Value.data(), entry->Value.size());
		}
		if (writer.Size() > MAXIMUM_FILE_BYTES) {
			error = "shared store image exceeds 256 MiB";
			return SharedStoreFileStatus::Malformed;
		}

		std::error_code failure;
		if (!path.parent_path().empty()) {
			std::filesystem::create_directories(path.parent_path(), failure);
			if (failure) {
				error = "could not create " + path.parent_path().string();
				return SharedStoreFileStatus::IoError;
			}
		}
		const std::filesystem::path temporary = path.string() + ".tmp";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output) {
				error = "could not write " + temporary.string();
				return SharedStoreFileStatus::IoError;
			}
			const auto bytes = writer.Bytes();
			output.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			if (!output) {
				error = "could not finish " + temporary.string();
				return SharedStoreFileStatus::IoError;
			}
		}

		std::filesystem::rename(temporary, path, failure);
		if (failure) {
			std::filesystem::remove(path, failure);
			failure.clear();
			std::filesystem::rename(temporary, path, failure);
		}
		if (failure) {
			error = "could not replace " + path.string();
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			return SharedStoreFileStatus::IoError;
		}
		return SharedStoreFileStatus::Ok;
	}

	SharedStoreFileStatus LoadSharedStoreFile(
		const std::filesystem::path &path,
		BusKind expectedStore,
		std::vector<SharedStoreEntry> &entries,
		std::string &error
	) {
		error.clear();
		std::error_code failure;
		if (!std::filesystem::exists(path, failure)) {
			return SharedStoreFileStatus::NotFound;
		}
		const uintmax_t length = std::filesystem::file_size(path, failure);
		if (failure || length > MAXIMUM_FILE_BYTES) {
			error = "shared store file is unreadable or exceeds 256 MiB";
			return SharedStoreFileStatus::IoError;
		}

		std::ifstream input(path, std::ios::binary);
		std::vector<std::byte> bytes(static_cast<size_t>(length));
		if (!input || (!bytes.empty() && !input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))) {
			error = "could not read " + path.string();
			return SharedStoreFileStatus::IoError;
		}

		core::ByteReader reader(bytes);
		if (reader.ReadUInt64() != MAGIC || reader.ReadUInt32() != FORMAT) {
			error = "not a supported shared store file";
			return SharedStoreFileStatus::Malformed;
		}
		const BusKind stored = static_cast<BusKind>(reader.ReadUInt8());
		if (!Supported(expectedStore) || stored != expectedStore) {
			error = "shared store file contains " +
					std::string(stored == BusKind::DataStore ? "DataStore" : "MemoryStore");
			return SharedStoreFileStatus::WrongStore;
		}
		const uint32_t count = reader.ReadUInt32();
		if (count > MAXIMUM_ENTRIES) {
			reader.Fail();
		}

		std::vector<SharedStoreEntry> loaded;
		loaded.reserve(reader.Failed() ? 0 : count);
		std::unordered_set<uint32_t> keys;
		for (uint32_t index = 0; index < count && !reader.Failed(); index++) {
			SharedStoreEntry entry;
			entry.Store = stored;
			entry.Key = reader.ReadName();
			entry.Version = reader.ReadUInt64();
			const uint32_t valueBytes = reader.ReadUInt32();
			const auto value = reader.ReadRawView(valueBytes);
			if (!entry.Key.IsValid() || !keys.insert(entry.Key.Id()).second ||
				(stored == BusKind::DataStore && entry.Version == 0) ||
				(stored == BusKind::MemoryStore && entry.Version != 0)) {
				reader.Fail();
				break;
			}
			entry.Value.assign(value.begin(), value.end());
			loaded.push_back(std::move(entry));
		}
		if (!reader.AtEnd()) {
			error = "shared store file is truncated or malformed";
			return SharedStoreFileStatus::Malformed;
		}
		entries.swap(loaded);
		return SharedStoreFileStatus::Ok;
	}
}
