#include <engine/assets/ContentHash.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/delivery/Cache.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::delivery {
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

		// Written to a temporary and renamed into place.
		//
		// A cache entry is addressed by the hash of its contents, so a
		// half-written file under the right name is a lie the next reader
		// believes until it hashes it - and two processes writing the same
		// entry at once is ordinary rather than exotic. A rename is atomic on
		// every filesystem this targets, so a reader sees the whole file or no
		// file.
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

	ContentCache::ContentCache(fs::path directory, uint64_t capacityBytes)
		: Base(std::move(directory)), Ceiling(capacityBytes) {}

	std::optional<ContentCache> ContentCache::Open(const fs::path &directory, uint64_t capacityBytes) {
		if (directory.empty()) {
			return std::nullopt;
		}

		std::error_code failure;
		fs::create_directories(directory, failure);
		if (!fs::is_directory(directory, failure)) {
			// An ordinary outcome on a read-only install. A client runs without
			// a cache rather than refusing to start - every fetch simply costs
			// the network.
			ENGINE_WARN(
				"delivery: no content cache at {} - every fetch will reach a source", directory.string()
			);
			return std::nullopt;
		}
		ContentCache cache(fs::canonical(directory, failure), capacityBytes);
		if (capacityBytes > 0) {
			// One walk, at start-up, so that stores do not each need one. An
			// uncapped cache never evicts and so never needs the figure.
			cache.Held = cache.Bytes();
		}
		return cache;
	}

	fs::path ContentCache::PathOf(const assets::ContentHash &root) const {
		const std::string hex = root.ToHex();
		return Base / hex.substr(0, 2) / hex;
	}

	std::optional<std::vector<std::byte>> ContentCache::Find(const assets::AssetEntry &asset) {
		ENGINE_PROFILE_CAT("ContentCache::Find", core::ProfileCategory::Assets);

		const fs::path path = PathOf(asset.Root);
		std::optional<std::vector<std::byte>> bytes = ReadWholeFile(path);
		if (!bytes) {
			core::Metrics::Count("delivery.cache.miss", 1.0);
			return std::nullopt;
		}

		// Against the chunk list and the tree, which is what an asset root
		// actually is. A hash of the whole would be the wrong comparison.
		if (!assets::VerifyAsset(asset, *bytes)) {
			// Deleted rather than returned or left alone. Leaving it makes
			// every later run pay the same failed read; returning it defeats
			// the point of addressing content by hash.
			std::error_code ignored;
			if (fs::remove(path, ignored)) {
				Held -= std::min(Held, static_cast<uint64_t>(bytes->size()));
			}
			core::Metrics::Count("delivery.cache.corrupt", 1.0);
			ENGINE_WARN("delivery: cached {} did not verify and was dropped", asset.Root.ToHex());
			return std::nullopt;
		}

		// Touched so eviction can tell what is being used. A read is what makes
		// an entry recently used, and without this the cache would evict by age
		// of arrival rather than by use.
		std::error_code ignored;
		fs::last_write_time(path, fs::file_time_type::clock::now(), ignored);

		core::Metrics::Count("delivery.cache.hit", 1.0);
		return bytes;
	}

	bool ContentCache::Store(const assets::AssetEntry &asset, std::span<const std::byte> bytes) {
		ENGINE_PROFILE_CAT("ContentCache::Store", core::ProfileCategory::Assets);

		if (!assets::VerifyAsset(asset, bytes)) {
			// A caller bug rather than a cache failure: everything reaching
			// here should already have been verified against the manifest.
			core::Metrics::Count("delivery.cache.refused", 1.0);
			return false;
		}
		if (Ceiling > 0 && static_cast<uint64_t>(bytes.size()) > Ceiling) {
			// One asset that evicts everything else on every store is worse
			// than not caching that asset at all - PreparedCache's rule.
			return false;
		}

		// **An entry already here is the same bytes, so it needs no room and is
		// not counted twice.** A content address cannot name two different
		// things, and a group arriving with a member this cache already holds
		// is ordinary rather than exotic - every bundle carrying a shared
		// texture does it. One `stat` buys both: nothing is evicted to make
		// space that is already taken, and the running total does not grow on a
		// write that added no bytes. Without it the total would only ever rise
		// and a cache that re-stored enough would evict itself empty.
		const bool holding = Contains(asset.Root);
		if (!holding) {
			MakeRoom(static_cast<uint64_t>(bytes.size()));
		}
		if (!WriteWholeFile(PathOf(asset.Root), bytes)) {
			return false;
		}
		if (!holding) {
			Held += static_cast<uint64_t>(bytes.size());
		}
		core::Metrics::Count("delivery.cache.stored", 1.0);
		return true;
	}

	bool ContentCache::Contains(const assets::ContentHash &root) const {
		std::error_code failure;
		return fs::is_regular_file(PathOf(root), failure);
	}

	uint64_t ContentCache::Bytes() const {
		uint64_t total = 0;
		std::error_code failure;
		for (const auto &entry : fs::recursive_directory_iterator(Base, failure)) {
			if (entry.is_regular_file(failure)) {
				total += entry.file_size(failure);
			}
		}
		return total;
	}

	size_t ContentCache::Count() const {
		size_t count = 0;
		std::error_code failure;
		for (const auto &entry : fs::recursive_directory_iterator(Base, failure)) {
			if (entry.is_regular_file(failure)) {
				++count;
			}
		}
		return count;
	}

	void ContentCache::MakeRoom(uint64_t incoming) {
		if (Ceiling == 0) {
			return;
		}

		// **`Held` decides whether to look; the directory decides what to
		// evict.** Until v0.19 every store walked the whole cache tree - three
		// syscalls per file already there - so caching N assets cost N squared
		// stats, inside `Pump`, at the tick barrier. The walk below is
		// unchanged and still the only thing eviction acts on; what changed is
		// that it is reached only when the running total says the ceiling is in
		// reach. Measured in `bench` over 1024 four-kilobyte assets stored into
		// a cache with room for all of them: 1.17 ms per store before, 16.4 us
		// after, against 16.3 us for the same stores into an uncapped cache,
		// which does no eviction work at all. The remaining figure is the write
		// and the verification hash, so the walk is gone rather than reduced.
		//
		// **A running total can drift and an index of what is here must not.**
		// Another process writing into this directory makes `Held` too small,
		// so the first scan happens a little late and the cache overshoots its
		// ceiling by whatever that process wrote; one deleting files makes it
		// too large, so a scan happens early and finds nothing to do. Neither
		// can hide a file from eviction, because eviction reads the directory
		// rather than the total - and both correct themselves here, where the
		// total is replaced by what was actually found. That is the whole
		// reason the cheap path is a *trigger* and not an answer.
		if (Held + incoming <= Ceiling) {
			return;
		}

		struct Entry {
			fs::path Path;
			fs::file_time_type Used;
			uint64_t Bytes = 0;
		};

		std::vector<Entry> entries;
		uint64_t total = 0;
		std::error_code failure;
		for (const auto &found : fs::recursive_directory_iterator(Base, failure)) {
			if (!found.is_regular_file(failure)) {
				continue;
			}
			const auto size = static_cast<uint64_t>(found.file_size(failure));
			entries.push_back(
				Entry{.Path = found.path(), .Used = found.last_write_time(failure), .Bytes = size}
			);
			total += size;
		}

		Held = total;
		if (total + incoming <= Ceiling) {
			return;
		}

		// **Evicted down to below the ceiling rather than exactly to it**, so
		// that the stores after this one find room without looking. A cache
		// whose working set is larger than it is sits at its ceiling for ever,
		// and evicting the minimum would put a walk on every store from then
		// on - which is the case a small cache on a handheld is always in.
		// Measured over 1024 four-kilobyte assets into a cache a quarter their
		// size: 647 us per store evicting the minimum, 36 us with an eighth of
		// the ceiling in hand. What it costs is an eighth of the cache thrown
		// away earlier than it had to be, which is content that is fetched
		// again rather than content that is lost.
		const uint64_t target = Ceiling - Ceiling / 8;

		// Oldest use first, over what the filesystem says is here rather than
		// over anything remembered. An in-memory index of the entries would be
		// a second source for a fact the directory already holds, and the run
		// where the two disagree is the run that evicts a file somebody else
		// wrote or misses one it should have taken.
		std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
			return left.Used < right.Used;
		});

		for (const Entry &entry : entries) {
			if (total + incoming <= target) {
				break;
			}
			std::error_code ignored;
			if (fs::remove(entry.Path, ignored)) {
				total -= std::min(total, entry.Bytes);
				core::Metrics::Count("delivery.cache.evicted", 1.0);
			}
		}
		Held = total;
	}

	void ContentCache::Clear() {
		std::error_code failure;
		for (const auto &entry : fs::directory_iterator(Base, failure)) {
			std::error_code ignored;
			fs::remove_all(entry.path(), ignored);
		}
		Held = 0;
	}
}
