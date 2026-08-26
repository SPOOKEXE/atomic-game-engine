// The on-disk content cache, which every fetch consults before any source.
//
// **`Compression.cpp` measures what a group costs in flight; this measures what
// it costs once it has landed.** A pump that split an arriving group stores each
// member here, and a pump that resolved a request reads one back out. Both are
// inside `Pump`, which a world calls at the tick barrier, so a cost that grows
// with the number of entries already cached is frame time that gets worse the
// longer the game is played.
//
// **The rows are stated per asset, and the interesting comparison is between
// them rather than against a clock.** Storing into a capped cache and storing
// into an uncapped one write the same bytes to the same disk; whatever separates
// them is bookkeeping, because an uncapped cache does no eviction work at all.
//
// **A thousand four-kilobyte assets is a small game's cache and a deliberately
// unflattering one.** The eviction bookkeeping is per entry and the content is
// per byte, so small entries are where the bookkeeping shows. A cache of a
// hundred large meshes would hide it completely, which is exactly why the
// reported problem went unnoticed.
//
// **These rows measure a filesystem as much as they measure this code.** Same
// caveat as `assets/benchmarks/Store.cpp`: absolute figures move by two orders
// of magnitude between an NVMe drive and a network mount, and what is
// comparable between machines is the shape.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/delivery/Cache.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

TEST_SUITE_ID("engine.delivery.bench.cache")

using engine::assets::AssetEntry;
using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::delivery::ContentCache;
using engine::testing::Consume;

namespace cache_bench {
	// Assets stored per sample.
	constexpr size_t ASSETS = 1024;

	// Bytes per asset. Small on purpose - see the note at the top.
	constexpr size_t ASSET_BYTES = 4 * 1024;

	// A ceiling nothing in these rows reaches, so the eviction *scan* is
	// measured without the eviction itself.
	constexpr uint64_t ROOMY = 64ull * 1024 * 1024;

	// A ceiling a quarter of the working set fits under, so eviction runs on
	// most stores.
	constexpr uint64_t TIGHT = ASSETS * ASSET_BYTES / 4;

	// A scratch tree that removes itself.
	//
	// Under the system temporary directory rather than the build tree, because
	// a benchmark that left files in `.cache/build` would be found by
	// `orphan-check` and reported as a stale artefact - which it would be.
	struct Tree {
		std::filesystem::path Root;

		explicit Tree(std::string_view purpose) {
			static int serial = 0;
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-bench-cache-" + std::string(purpose) + "-" + std::to_string(++serial));
			std::error_code failure;
			std::filesystem::remove_all(Root, failure);
			std::filesystem::create_directories(Root, failure);
		}

		~Tree() {
			std::error_code failure;
			std::filesystem::remove_all(Root, failure);
		}

		Tree(const Tree &) = delete;
		Tree &operator=(const Tree &) = delete;
	};

	// Distinct, incompressible content for each asset, built once.
	const std::vector<std::vector<std::byte>> &Bodies() {
		static const std::vector<std::vector<std::byte>> built = [] {
			std::vector<std::vector<std::byte>> bodies;
			bodies.reserve(ASSETS);
			uint64_t state = 0x9E3779B97F4A7C15ull;
			for (size_t asset = 0; asset < ASSETS; asset++) {
				std::vector<std::byte> bytes(ASSET_BYTES);
				for (size_t at = 0; at < ASSET_BYTES; at += 8) {
					state = state * 6364136223846793005ull + 1442695040888963407ull;
					for (size_t byte = 0; byte < 8; byte++) {
						bytes[at + byte] = static_cast<std::byte>((state >> (byte * 8)) & 0xFF);
					}
				}
				bodies.push_back(std::move(bytes));
			}
			return bodies;
		}();
		return built;
	}

	// A manifest describing every body as a one-chunk asset.
	//
	// Built through `AddAsset` rather than by filling in `AssetEntry` by hand,
	// so the entries carry exactly the roots and totals the format computes -
	// `ContentCache` verifies against them and would refuse anything else.
	const Manifest &Catalogue() {
		static const Manifest built = [] {
			Manifest manifest;
			const std::vector<std::vector<std::byte>> &bodies = Bodies();
			for (size_t asset = 0; asset < ASSETS; asset++) {
				manifest.AddAsset(
					"content/cached-" + std::to_string(asset) + ".abin",
					AssetKind::Mesh,
					{ChunkEntry{.Hash = Hasher::Of(bodies[asset]), .Bytes = ASSET_BYTES}}
				);
			}
			return manifest;
		}();
		return built;
	}

	// The entry for body `index`, which is not the manifest's `index` because
	// the manifest is in name order.
	const AssetEntry &Entry(size_t index) {
		static const std::vector<const AssetEntry *> order = [] {
			std::vector<const AssetEntry *> found;
			found.reserve(ASSETS);
			for (size_t asset = 0; asset < ASSETS; asset++) {
				found.push_back(Catalogue().Find("content/cached-" + std::to_string(asset) + ".abin"));
			}
			return found;
		}();
		return *order[index];
	}

	// Stores every body into a fresh cache with the given ceiling.
	size_t FillFresh(uint64_t ceiling, std::string_view purpose) {
		Tree tree(purpose);
		std::optional<ContentCache> cache = ContentCache::Open(tree.Root, ceiling);
		const std::vector<std::vector<std::byte>> &bodies = Bodies();
		size_t stored = 0;
		for (size_t asset = 0; asset < ASSETS; asset++) {
			stored += cache->Store(Entry(asset), bodies[asset]) ? 1 : 0;
		}
		return stored;
	}

	// A cache holding every asset, opened once and kept for the read rows.
	//
	// The tree is a static local declared first, so it is created before the
	// cache and destroyed after it.
	ContentCache &Filled() {
		static Tree tree("filled");
		static std::optional<ContentCache> cache = [] {
			std::optional<ContentCache> opened = ContentCache::Open(tree.Root, ROOMY);
			const std::vector<std::vector<std::byte>> &bodies = Bodies();
			for (size_t asset = 0; asset < ASSETS; asset++) {
				opened->Store(Entry(asset), bodies[asset]);
			}
			return opened;
		}();
		return *cache;
	}
}

using namespace cache_bench;

// --- storing ------------------------------------------------------------------

BENCH_PER_ITEM("Store · 1024 assets into a cache with a 64 MiB ceiling", ASSETS) {
	// **The pump path.** Every member of an arriving group is stored here, and
	// nothing in this row ever evicts: eight megabytes of content under a
	// sixty-four megabyte ceiling.
	//
	// So whatever this row costs over the uncapped row below is the price of
	// *deciding* not to evict, paid once per asset stored, on a frame.
	Consume(FillFresh(ROOMY, "roomy"));
}

BENCH_PER_ITEM("Store · 1024 assets into a cache with no ceiling", ASSETS) {
	// **The control.** A capacity of zero means unlimited, which short-circuits
	// eviction entirely, so this row is the verification hash plus the write and
	// nothing else. Subtract it from the row above.
	Consume(FillFresh(0, "uncapped"));
}

BENCH_PER_ITEM("Store · 1024 assets into a cache a quarter their size", ASSETS) {
	// Eviction actually running: three quarters of what is stored is thrown
	// away again, oldest use first. A cache sized well below a game's working
	// set is a real deployment rather than a pathological one - a handheld with
	// a small ceiling behaves exactly like this.
	Consume(FillFresh(TIGHT, "tight"));
}

// --- reading ------------------------------------------------------------------

BENCH_PER_ITEM("Find · 1024 hits, each verified", ASSETS) {
	// What "local cache first" costs when it works. A read, a BLAKE3 pass over
	// what came back and a touch of the file's timestamp so eviction can tell
	// what is being used.
	ContentCache &cache = Filled();
	size_t bytes = 0;
	for (size_t asset = 0; asset < ASSETS; asset++) {
		const std::optional<std::vector<std::byte>> found = cache.Find(Entry(asset));
		bytes += found.has_value() ? found->size() : 0;
	}
	Consume(bytes);
}

BENCH_PER_ITEM("Contains · 1024 probes for assets that are there", ASSETS) {
	// A `stat` and nothing else, which is what a progress readout asks. It has
	// to stay far below the `Find` row: a presence check that read the file
	// would make a diagnostic cost as much as a fetch.
	ContentCache &cache = Filled();
	size_t present = 0;
	for (size_t asset = 0; asset < ASSETS; asset++) {
		present += cache.Contains(Entry(asset).Root) ? 1 : 0;
	}
	Consume(present);
}
