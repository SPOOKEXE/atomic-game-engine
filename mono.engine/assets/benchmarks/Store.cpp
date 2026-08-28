// Chunks going to and coming off a disk, which is the one path here that waits.
//
// **`Content.cpp` measures the pipeline with the disk taken out and this puts
// it back.** Hashing and chunking are pure functions over a buffer somebody
// already had; a publish reads every file in a game and writes a chunk file per
// distinct chunk, and a client verifying a download reads every one of them
// back. Those are the operations that take minutes rather than milliseconds,
// and neither of them appears anywhere in a suite that never opens a file.
//
// **A file per chunk is a decision with a cost, and this is where the cost is.**
// The layout is one file named by its own hash under two hex characters of
// fan-out, chosen because a hundred thousand files in one directory is slow to
// enumerate everywhere and pathological on some filesystems. What the fan-out
// does not remove is the per-file syscall floor - an open, a write, a close per
// chunk - and at 64 KiB a chunk that floor is a fixed tax on every byte the
// engine ships. The row that writes chunks and the row that writes the same
// bytes as one file are what price it.
//
// **Every read verifies, and the verification is not free.** A chunk's name is
// the hash of its bytes, so checking costs one BLAKE3 pass and catches a
// corrupt disk, a partial write and a tampered store with the same check. The
// read rows are therefore a read plus a hash, and `Content.cpp`'s hashing rows
// are what says how much of the figure is which.
//
// **These rows measure a filesystem as much as they measure this code**, which
// is unusual here and is stated rather than hidden. A run on an NVMe drive and
// a run on a network mount are different by two orders of magnitude and neither
// is wrong. What is comparable between machines is the *shape*: dedup should
// cost far less than a write, `Contains` far less than a read, and a
// second read of the same chunk far less than the first, because by then the
// page cache has it. A row that breaks one of those relations is this code's
// problem on any disk.
//
// **There is no asynchronous file path in this engine and so there is none
// measured here.** Nothing in `assets`, `delivery`, `mono.client` or
// `mono.server` reads a file off the calling thread; a publish blocks the tool
// and a fetch blocks whoever asked. That is a defensible design at this size
// and it is a fact worth having written down beside the numbers that would
// justify changing it, because the argument for a background reader is exactly
// the gap between these figures and a frame budget.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

TEST_SUITE_ID("engine.assets.bench.store")

using engine::assets::AssetEntry;
using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::testing::Consume;

namespace fs = std::filesystem;

namespace store_bench {
	// The default chunk size, which is what the store actually holds.
	constexpr size_t CHUNK_BYTES = 64 * 1024;

	// Chunks per sample. Thirty-two megabytes, which is a small game's worth of
	// one asset kind and enough that the per-file cost is not lost in the
	// noise of opening the directory.
	constexpr size_t CHUNKS = 512;

	// A scratch tree that removes itself.
	//
	// Under the system temporary directory rather than the build tree, because
	// a benchmark that left files in `.cache/build` would be found by
	// `orphan-check` and reported as a stale artefact - which it would be.
	struct Tree {
		fs::path Root;

		explicit Tree(std::string_view purpose) {
			static int serial = 0;
			Root = fs::temp_directory_path() /
				   ("atomic-bench-store-" + std::string(purpose) + "-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
			fs::create_directories(Root, failure);
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		Tree(const Tree &) = delete;
		Tree &operator=(const Tree &) = delete;
	};

	// Distinct chunk contents, built once.
	//
	// **Distinct, and not compressible.** Chunks that shared bytes would be
	// deduplicated into one file and the write row would measure the dedup path
	// by accident; chunks of zeroes would let a filesystem with sparse files
	// write nothing at all.
	const std::vector<std::vector<std::byte>> &Chunks() {
		static const std::vector<std::vector<std::byte>> built = [] {
			std::vector<std::vector<std::byte>> chunks;
			chunks.reserve(CHUNKS);
			uint64_t state = 0x243F6A8885A308D3ull;
			for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
				std::vector<std::byte> bytes(CHUNK_BYTES);
				for (size_t at = 0; at < CHUNK_BYTES; at += 8) {
					state = state * 6364136223846793005ull + 1442695040888963407ull;
					for (size_t byte = 0; byte < 8; byte++) {
						bytes[at + byte] = static_cast<std::byte>((state >> (byte * 8)) & 0xFF);
					}
				}
				chunks.push_back(std::move(bytes));
			}
			return chunks;
		}();
		return built;
	}

	// The name each chunk will be stored under, hashed once.
	const std::vector<ContentHash> &Names() {
		static const std::vector<ContentHash> built = [] {
			std::vector<ContentHash> names;
			names.reserve(CHUNKS);
			for (const std::vector<std::byte> &chunk : Chunks()) {
				names.push_back(Hasher::Of(chunk));
			}
			return names;
		}();
		return built;
	}

	// A store holding every chunk, opened once and kept for the read rows.
	//
	// The tree is a static local declared first, so it is created before the
	// store and destroyed after it - which is what makes the scratch directory
	// outlive every row that reads from it and still be swept at exit.
	ChunkStore &Filled() {
		static Tree tree("filled");
		static std::optional<ChunkStore> store = [] {
			std::optional<ChunkStore> opened = ChunkStore::Open(tree.Root, true);
			const std::vector<ContentHash> &names = Names();
			const std::vector<std::vector<std::byte>> &chunks = Chunks();
			for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
				opened->Write(names[chunk], chunks[chunk]);
			}
			return opened;
		}();
		return *store;
	}

	// Chunks per asset for the reassembly rows. Sixty-four 64 KiB chunks is four
	// megabytes, which is an ordinary character mesh with its textures beside it.
	constexpr size_t ASSET_CHUNKS = 64;

	// A manifest describing the store's first `ASSET_CHUNKS` chunks as one
	// asset, in stream order.
	//
	// Built through `AddAsset` rather than by filling in an `AssetEntry` by
	// hand, so the root and the total are exactly what the format computes -
	// `ReadAsset` verifies against them and would refuse anything else.
	const Manifest &Catalogue() {
		static const Manifest built = [] {
			Manifest manifest;
			std::vector<ChunkEntry> entries;
			entries.reserve(ASSET_CHUNKS);
			const std::vector<ContentHash> &names = Names();
			for (size_t chunk = 0; chunk < ASSET_CHUNKS; chunk++) {
				entries.push_back(ChunkEntry{.Hash = names[chunk], .Bytes = CHUNK_BYTES});
			}
			manifest.AddAsset("content/reassembled.abin", AssetKind::Mesh, std::move(entries));
			return manifest;
		}();
		return built;
	}

	// That asset's entry.
	const AssetEntry &Asset() {
		return Catalogue().Assets().front();
	}

	// The same asset as one buffer, for the row that verifies without a disk.
	const std::vector<std::byte> &Reassembled() {
		static const std::vector<std::byte> built = [] {
			std::vector<std::byte> whole;
			whole.reserve(ASSET_CHUNKS * CHUNK_BYTES);
			const std::vector<std::vector<std::byte>> &chunks = Chunks();
			for (size_t chunk = 0; chunk < ASSET_CHUNKS; chunk++) {
				whole.insert(whole.end(), chunks[chunk].begin(), chunks[chunk].end());
			}
			return whole;
		}();
		return built;
	}
}

using namespace store_bench;

// --- writing ------------------------------------------------------------------

BENCH_PER_ITEM("Write · 512 fresh 64 KiB chunks", CHUNKS) {
	// **The publish path**, and the figure that decides how long publishing a
	// game takes. One `open`, one `write`, one `close` per chunk plus the
	// directory the fan-out puts it in. Read it as bytes per second by dividing
	// 64 KiB by the row, and compare against what the drive does for one large
	// sequential file - the difference is the per-file tax.
	Tree tree("write");
	std::optional<ChunkStore> store = ChunkStore::Open(tree.Root, true);
	const std::vector<ContentHash> &names = Names();
	const std::vector<std::vector<std::byte>> &chunks = Chunks();
	size_t written = 0;
	for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
		written += store->Write(names[chunk], chunks[chunk]) ? 1 : 0;
	}
	Consume(written);
}

BENCH_PER_ITEM("Write · 512 chunks the store already holds", CHUNKS) {
	// **Dedup, which is the property content addressing exists for.** Two files
	// sharing bytes share chunks and the second write is a no-op, so publishing
	// a game a second time after changing one texture should cost almost
	// nothing. If this row is anywhere near the fresh-write row, the no-op is
	// not a no-op and every republish rewrites the whole store.
	//
	// It is not, and it is also not free: as measured it costs several times
	// the `Contains` row below, which is the same question answered by the same
	// filesystem. Whatever the difference is, it is paid once per chunk of
	// every republish, and the two rows sitting next to each other is what
	// makes it a number rather than an assumption.
	ChunkStore &store = Filled();
	const std::vector<ContentHash> &names = Names();
	const std::vector<std::vector<std::byte>> &chunks = Chunks();
	size_t written = 0;
	for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
		written += store.Write(names[chunk], chunks[chunk]) ? 1 : 0;
	}
	Consume(written);
}

// --- reading ------------------------------------------------------------------

BENCH_PER_ITEM("Read · 512 64 KiB chunks, each verified", CHUNKS) {
	// **The client path.** Every read hashes what it got and compares against
	// the name it asked for, so this is a read plus a BLAKE3 pass over the same
	// bytes. Subtract `engine.assets.bench.content`'s 64 KiB hashing row to see
	// what the file system contributed.
	ChunkStore &store = Filled();
	const std::vector<ContentHash> &names = Names();
	size_t bytes = 0;
	for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
		const std::optional<std::vector<std::byte>> read = store.Read(names[chunk]);
		bytes += read.has_value() ? read->size() : 0;
	}
	Consume(bytes);
}

BENCH_PER_ITEM("Read · 512 chunks that are not there", CHUNKS) {
	// A miss, which is what a client asking for content this origin does not
	// have gets, and what a scanner walking hash space gets every time. It has
	// to be a failed `open` and nothing else - a miss that cost a directory
	// walk would let a stranger make the origin work by asking for things that
	// do not exist.
	ChunkStore &store = Filled();
	size_t found = 0;
	for (uint64_t chunk = 0; chunk < CHUNKS; chunk++) {
		ContentHash absent;
		for (size_t byte = 0; byte < absent.Digest.size(); byte++) {
			absent.Digest[byte] = static_cast<uint8_t>((chunk >> (byte % 8 * 8)) ^ 0xEE);
		}
		found += store.Read(absent).has_value() ? 1 : 0;
	}
	Consume(found);
}

BENCH_PER_ITEM("Contains · 512 probes for chunks that are there", CHUNKS) {
	// **What a publisher does before deciding to write.** It is a `stat` rather
	// than a read, so it must be far cheaper than the read row - and if it is
	// not, the dedup check is reading the chunk in order to decide not to write
	// it.
	ChunkStore &store = Filled();
	const std::vector<ContentHash> &names = Names();
	size_t present = 0;
	for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
		present += store.Contains(names[chunk]) ? 1 : 0;
	}
	Consume(present);
}

// --- the per-file tax ---------------------------------------------------------

BENCH_PER_ITEM("Control · the same 32 MiB written as one file", CHUNKS) {
	// **Not a measurement of this code, and that is what makes it useful.** The
	// same bytes, one `open` and one `close`, so the difference between this
	// row and the fresh-write row is exactly what a file per chunk costs on
	// this machine. That number is the price of dedup and patching, and it is
	// worth knowing rather than assuming - the chunk layout is a trade and
	// this is the side of it nothing else states.
	Tree tree("control");
	std::ofstream file(tree.Root / "whole", std::ios::binary);
	size_t bytes = 0;
	for (const std::vector<std::byte> &chunk : Chunks()) {
		file.write(reinterpret_cast<const char *>(chunk.data()), std::streamsize(chunk.size()));
		bytes += chunk.size();
	}
	file.flush();
	Consume(bytes);
}

// --- reassembly ---------------------------------------------------------------

BENCH_PER_ITEM("ChunkStore::ReadAsset · a 4 MiB asset in 64 chunks", ASSET_CHUNKS) {
	// **The client path end to end**, and the one inside a tick: a bundle read
	// out of a local store is cut into assets by `ChunkStore::ReadBundle`, which
	// calls this per member, from `delivery::Client::Start`, from `Pump`.
	//
	// Read against the two rows below. `Read` is what the chunks cost coming off
	// the disk with one BLAKE3 pass each; `VerifyAsset` is a second pass over
	// the same bytes with no disk in it at all. If this row is close to their
	// sum, the second pass is being paid.
	ChunkStore &store = Filled();
	const std::optional<std::vector<std::byte>> whole = store.ReadAsset(Asset());
	Consume(whole.has_value() ? whole->size() : 0);
}

BENCH_PER_ITEM("ChunkStore::Read · the same 64 chunks, not reassembled", ASSET_CHUNKS) {
	// The disk half of the row above, with the same verification per chunk and
	// without the concatenation or anything after it.
	ChunkStore &store = Filled();
	size_t bytes = 0;
	for (const ChunkEntry &chunk : Asset().Chunks) {
		const std::optional<std::vector<std::byte>> read = store.Read(chunk.Hash);
		bytes += read.has_value() ? read->size() : 0;
	}
	Consume(bytes);
}

BENCH_PER_ITEM("VerifyAsset · 4 MiB already in memory", ASSET_CHUNKS) {
	// **No disk.** One BLAKE3 pass over the asset in chunk-sized pieces plus the
	// tree over their hashes, which is the check `delivery::Client` makes
	// against bytes off a wire - where it is the only check there is - and which
	// `ChunkStore::ReadAsset` used to repeat over bytes each of whose chunks had
	// just been hashed on the way in.
	Consume(engine::assets::VerifyAsset(Asset(), Reassembled()) ? 1 : 0);
}
