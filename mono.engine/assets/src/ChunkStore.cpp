#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/HashTree.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace engine::assets {
	namespace {
		namespace fs = std::filesystem;

		std::optional<std::vector<std::byte>> ReadWholeFile(const fs::path &path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return std::nullopt;
			}
			const std::streamoff size = file.tellg();
			if (size < 0) {
				return std::nullopt;
			}
			file.seekg(0);

			std::vector<std::byte> bytes(static_cast<size_t>(size));
			if (size > 0) {
				file.read(reinterpret_cast<char *>(bytes.data()), size);
				if (!file) {
					return std::nullopt;
				}
			}
			return bytes;
		}

		// Staged and renamed, so a reader sees a whole file or no file.
		//
		// A chunk is named by the hash of its contents, so a half-written file
		// under the right name is a claim the next reader has to disprove - and
		// two publishers writing the same chunk at once is ordinary rather than
		// exotic, because content addressing means they often will.
		bool WriteWholeFile(const fs::path &path, std::span<const std::byte> bytes) {
			std::error_code failure;
			fs::create_directories(path.parent_path(), failure);

			const fs::path staging = path.string() + ".partial";
			{
				std::ofstream file(staging, std::ios::binary | std::ios::trunc);
				if (!file) {
					return false;
				}
				if (!bytes.empty()) {
					file.write(
						reinterpret_cast<const char *>(bytes.data()),
						static_cast<std::streamsize>(bytes.size())
					);
				}
				if (!file) {
					return false;
				}
			}

			fs::rename(staging, path, failure);
			if (failure) {
				std::error_code ignored;
				fs::remove(staging, ignored);
				return false;
			}
			return true;
		}
	}

	ChunkStore::ChunkStore(fs::path directory) : Base(std::move(directory)) {}

	std::optional<ChunkStore> ChunkStore::Open(const fs::path &directory, bool create) {
		if (directory.empty()) {
			return std::nullopt;
		}

		std::error_code failure;
		if (create) {
			fs::create_directories(directory / "chunks", failure);
		}
		if (!fs::is_directory(directory, failure)) {
			// A reader pointed at a path that is not there fails here rather
			// than creating an empty store and then reporting every asset as
			// missing - which reads as "the content is gone" instead of "the
			// path is wrong".
			return std::nullopt;
		}
		return ChunkStore(fs::weakly_canonical(directory, failure));
	}

	fs::path ChunkStore::PathOf(const ContentHash &hash) const {
		const std::string hex = hash.ToHex();
		return Base / "chunks" / hex.substr(0, 2) / hex;
	}

	std::optional<std::vector<std::byte>> ChunkStore::Read(const ContentHash &hash) const {
		std::optional<std::vector<std::byte>> bytes = ReadWholeFile(PathOf(hash));
		if (!bytes) {
			return std::nullopt;
		}
		if (Hasher::Of(*bytes) != hash) {
			// The name is the hash, so this is a corrupt disk, a partial write
			// or a tampered store - and catching it here says *which chunk*,
			// where catching it at the asset root would only say that the asset
			// was wrong.
			core::Metrics::Count("assets.chunkstore.corrupt", 1.0);
			return std::nullopt;
		}
		return bytes;
	}

	bool ChunkStore::Write(const ContentHash &hash, std::span<const std::byte> bytes) {
		if (Hasher::Of(bytes) != hash) {
			core::Metrics::Count("assets.chunkstore.refused", 1.0);
			return false;
		}
		if (Contains(hash)) {
			// Two assets sharing a chunk is the point of content addressing, so
			// a publisher should not have to check first.
			return true;
		}
		return WriteWholeFile(PathOf(hash), bytes);
	}

	bool ChunkStore::Contains(const ContentHash &hash) const {
		std::error_code failure;
		return fs::is_regular_file(PathOf(hash), failure);
	}

	std::optional<std::vector<std::byte>> ChunkStore::ReadAsset(const AssetEntry &asset) const {
		ENGINE_PROFILE_CAT("ChunkStore::ReadAsset", core::ProfileCategory::Assets);

		std::vector<std::byte> whole;
		whole.reserve(static_cast<size_t>(asset.TotalBytes));

		for (const ChunkEntry &chunk : asset.Chunks) {
			std::optional<std::vector<std::byte>> bytes = Read(chunk.Hash);
			if (!bytes || bytes->size() != chunk.Bytes) {
				// The manifest records each chunk's length as well as its hash,
				// so a length that disagrees is caught before the bytes are
				// concatenated into something the caller would then have to
				// unpick.
				return std::nullopt;
			}
			whole.insert(whole.end(), bytes->begin(), bytes->end());
		}

		// Against the chunk list and the tree, which is what an asset root is.
		// One implementation of that check - `assets::VerifyAsset` - because the
		// delivery client makes the identical one against bytes off a wire and
		// against bytes out of its own cache.
		if (!VerifyAsset(asset, whole)) {
			core::Metrics::Count("assets.chunkstore.corrupt", 1.0);
			return std::nullopt;
		}
		return whole;
	}

	std::optional<std::vector<std::byte>>
	ChunkStore::ReadBundle(const Manifest &manifest, const BundleEntry &bundle) const {
		ENGINE_PROFILE_CAT("ChunkStore::ReadBundle", core::ProfileCategory::Assets);

		std::vector<std::byte> payload;
		payload.reserve(static_cast<size_t>(bundle.TotalBytes));

		// Member order, which is `Manifest::SliceOf`'s definition of where each
		// asset sits. One implementation of the layout, used by the origin
		// building a group and by the client splitting one.
		for (const ContentHash &member : bundle.Assets) {
			const AssetEntry *const asset = manifest.FindByRoot(member);
			if (asset == nullptr) {
				return std::nullopt;
			}
			std::optional<std::vector<std::byte>> bytes = ReadAsset(*asset);
			if (!bytes) {
				return std::nullopt;
			}
			payload.insert(payload.end(), bytes->begin(), bytes->end());
		}

		if (payload.size() != bundle.TotalBytes) {
			return std::nullopt;
		}
		return payload;
	}

	std::optional<Manifest> ChunkStore::ReadManifest(SignatureBytes &signature) const {
		std::optional<std::vector<std::byte>> bytes = ReadWholeFile(Base / MANIFEST_FILE);
		if (!bytes || bytes->size() <= SignatureBytes::BYTES) {
			return std::nullopt;
		}

		std::memcpy(signature.Value.data(), bytes->data(), SignatureBytes::BYTES);

		core::ByteReader reader(
			std::span<const std::byte>(
				bytes->data() + SignatureBytes::BYTES, bytes->size() - SignatureBytes::BYTES
			)
		);
		return Manifest::Read(reader);
	}

	bool ChunkStore::WriteManifest(const Manifest &manifest, const SignatureBytes &signature) {
		core::ByteWriter writer;
		manifest.Write(writer);
		const std::span<const std::byte> body = writer.Bytes();

		std::vector<std::byte> file;
		file.reserve(SignatureBytes::BYTES + body.size());
		// Signature first, so a reader knows what to verify before it has
		// parsed anything.
		const auto *const raw = reinterpret_cast<const std::byte *>(signature.Value.data());
		file.insert(file.end(), raw, raw + SignatureBytes::BYTES);
		file.insert(file.end(), body.begin(), body.end());

		return WriteWholeFile(Base / MANIFEST_FILE, file);
	}

	std::optional<std::vector<std::byte>> ChunkStore::ReadDictionary() const {
		return ReadWholeFile(Base / DICTIONARY_FILE);
	}

	bool ChunkStore::WriteDictionary(std::span<const std::byte> bytes) {
		return WriteWholeFile(Base / DICTIONARY_FILE, bytes);
	}

	size_t ChunkStore::Count() const {
		size_t count = 0;
		std::error_code failure;
		for (const auto &entry : fs::recursive_directory_iterator(Base / "chunks", failure)) {
			if (entry.is_regular_file(failure)) {
				++count;
			}
		}
		return count;
	}

	uint64_t ChunkStore::Bytes() const {
		uint64_t total = 0;
		std::error_code failure;
		for (const auto &entry : fs::recursive_directory_iterator(Base / "chunks", failure)) {
			if (entry.is_regular_file(failure)) {
				total += entry.file_size(failure);
			}
		}
		return total;
	}
}
