// Throughput, in the one module where throughput is the whole question.
//
// **Every row here should be read as megabytes per second, not nanoseconds.**
// Publishing a build hashes and chunks every asset in it; a client verifying a
// download hashes every chunk it received. Those are gigabyte-scale operations
// where nothing else in this engine is, so the useful figure is a rate and the
// useful comparison is against a disk — a pipeline that hashes at 300 MB/s on a
// drive that reads at 3 GB/s has turned an I/O-bound job into a CPU-bound one.
//
// The buffer sizes bracket that. 64 KiB is one chunk, which is what a client
// verifies one at a time; 16 MiB is a texture or a model, which is what a
// publisher hands to the chunker in one go. If the per-byte cost differs
// between them, the difference is the per-call setup and it tells a streaming
// caller how small a piece is worth feeding in.
//
// **The chunker is the row with a real algorithmic risk**, and it is the reason
// this file exists rather than a comment saying "BLAKE3 is fast". Content-
// defined chunking is a rolling hash over every byte with a mask test per byte;
// done naively it is several times dearer than the cryptographic hash that
// follows it, which is a surprising and completely invisible way for a
// publishing pipeline to become slow.

#include <engine/assets/Chunker.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/HashTree.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.assets.bench.content")

using engine::assets::ChunkLimits;
using engine::assets::Chunker;
using engine::assets::ChunkSpan;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::HashTree;
using engine::testing::Consume;

namespace content_bench {

	// One chunk at the default target size.
	constexpr size_t CHUNK_BYTES = 64 * 1024;

	// One asset. A texture or a model, and the unit a publisher chunks.
	constexpr size_t ASSET_BYTES = 16 * 1024 * 1024;

	// Bytes that do not compress and do not repeat, which is what a chunker
	// wants to be measured on.
	//
	// **Zeroes would be a lie in both directions.** A rolling hash over a
	// constant stream never finds a boundary, so the chunker would emit maximum
	// -size chunks and take the cheap path every time; and a cryptographic hash
	// over zeroes is the same speed but the result tells you nothing about
	// whether the buffer was ever really read. This is a cheap
	// non-cryptographic scramble — it only has to look like data to a mask test.
	const std::vector<std::byte> &Bytes(size_t count) {
		static std::vector<std::pair<size_t, std::vector<std::byte>>> built;
		for (const auto &[size, data] : built) {
			if (size == count) {
				return data;
			}
		}

		std::vector<std::byte> made(count);
		uint64_t state = 0x2545'F491'4F6C'DD1Dull;
		for (size_t index = 0; index < made.size(); index++) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			made[index] = static_cast<std::byte>(state & 0xFFull);
		}
		built.emplace_back(count, std::move(made));
		return built.back().second;
	}

	// Leaf hashes for the tree rows, one per chunk of a large asset.
	const std::vector<ContentHash> &Leaves(size_t count) {
		static std::vector<std::pair<size_t, std::vector<ContentHash>>> built;
		for (const auto &[size, leaves] : built) {
			if (size == count) {
				return leaves;
			}
		}

		std::vector<ContentHash> made;
		made.reserve(count);
		const std::vector<std::byte> &source = Bytes(CHUNK_BYTES);
		for (size_t index = 0; index < count; index++) {
			// Distinct leaves: a tree over identical hashes would collapse into
			// the same combine at every level and measure one cache line.
			std::vector<std::byte> piece(source.begin(), source.begin() + 64);
			piece[0] = static_cast<std::byte>(index & 0xFFu);
			piece[1] = static_cast<std::byte>((index >> 8) & 0xFFu);
			made.push_back(Hasher::Of(piece));
		}
		built.emplace_back(count, std::move(made));
		return built.back().second;
	}
}

using namespace content_bench;

// --- hashing ------------------------------------------------------------------
//
// One iteration is one **kibibyte**, so the reported figure converts to
// throughput by dividing: 1 ns/KiB is roughly 1 TB/s, 1000 ns/KiB is roughly
// 1 GB/s. Chosen over per-byte because a per-byte figure at these speeds
// rounds to zero and stops being a measurement.

BENCH("Hasher::Of · 64 KiB", CHUNK_BYTES / 1024) {
	const std::vector<std::byte> &data = Bytes(CHUNK_BYTES);
	Consume(Hasher::Of(data).Digest[0]);
}

BENCH("Hasher::Of · 16 MiB", ASSET_BYTES / 1024) {
	// A whole asset in one call. Against the 64 KiB row, the difference is the
	// per-call setup amortised over 256 times the data — so if this row is much
	// cheaper per kibibyte, a caller hashing chunk by chunk is paying that setup
	// once per chunk and should be using the streaming form instead.
	const std::vector<std::byte> &data = Bytes(ASSET_BYTES);
	Consume(Hasher::Of(data).Digest[0]);
}

BENCH("Hasher::Of · 256 tiny buffers", 256) {
	// The per-call floor. A manifest hashes many short things — names, entries,
	// roots — and at that size the setup is the entire cost. This row is what a
	// pipeline pays for granularity.
	static const std::vector<std::byte> tiny(64);
	for (size_t index = 0; index < 256; index++) {
		Consume(Hasher::Of(tiny).Digest[0]);
	}
}

// --- chunking -----------------------------------------------------------------
//
// **Read against the hashing rows at the same size.** A publisher does both to
// every byte it ships, so the two add. Content-defined chunking that costs more
// than the cryptographic hash it feeds is the failure mode to look for, and the
// pair of rows is what makes it visible rather than a suspicion.

BENCH("Chunker::Split · 16 MiB", ASSET_BYTES / 1024) {
	static const Chunker chunker;
	const std::vector<std::byte> &data = Bytes(ASSET_BYTES);
	Consume(chunker.Split(data).size());
}

BENCH("Chunker::NextBoundary · 16 MiB one chunk at a time", ASSET_BYTES / 1024) {
	// The streaming primitive, which is what a caller cutting a file without
	// holding it all uses. It should come out at the same per-kibibyte cost as
	// `Split`; if it is dearer, `Split` has an amortisation the streaming path
	// cannot get and a streaming publisher is paying for the convenience.
	static const Chunker chunker;
	const std::vector<std::byte> &data = Bytes(ASSET_BYTES);
	std::span<const std::byte> rest(data);
	size_t chunks = 0;
	while (!rest.empty()) {
		const size_t bytes = chunker.NextBoundary(rest);
		if (bytes == 0) {
			break;
		}
		rest = rest.subspan(bytes);
		chunks++;
	}
	Consume(chunks);
}

BENCH("Chunker::Split · 16 MiB of zeroes", ASSET_BYTES / 1024) {
	// **The degenerate stream, and it is not a contrived one** — a sparse file,
	// a zero-filled texture mip, a padded archive. The rolling hash never finds
	// a boundary, so every chunk is forced at `MaximumBytes`.
	//
	// **This row is dearer than the real-data one, and that is the correct
	// result rather than a regression.** A content-defined chunker has to hash
	// every byte — you cannot know a boundary is absent without looking — so
	// there is no early-out to be had here and the two rows once reported the
	// same figure. What separates them now is the warm-up window: `NextBoundary`
	// only feeds its hash for the 64 bytes before `MinimumBytes` rather than
	// from byte zero, so the skipped share is fixed per chunk while the tested
	// share grows with how long the chunk runs. A forced 256 KiB chunk therefore
	// skips proportionally less than a found 64 KiB one.
	//
	// The gap between these two rows is that saving, read from the other side.
	static const std::vector<std::byte> zeroes(ASSET_BYTES);
	static const Chunker chunker;
	Consume(chunker.Split(zeroes).size());
}

BENCH("Chunker::Split · 16 MiB with a small envelope", ASSET_BYTES / 1024) {
	// Eight times as many chunks over the same bytes. The per-byte rolling hash
	// is unchanged, so anything this row costs above the default one is
	// per-chunk: the span push, the bookkeeping, the mask reset. That is the
	// number that says what a smaller target size costs before any dedup
	// benefit is counted, and `ChunkLimits`' own comment admits there is no
	// measurement behind the defaults yet. This is half of it.
	static const Chunker chunker{[] {
		ChunkLimits limits;
		limits.MinimumBytes = 2 * 1024;
		limits.TargetBytes = 8 * 1024;
		limits.MaximumBytes = 32 * 1024;
		return limits;
	}()};
	const std::vector<std::byte> &data = Bytes(ASSET_BYTES);
	Consume(chunker.Split(data).size());
}

// --- the publishing pipeline --------------------------------------------------

BENCH("publish · 16 MiB chunked then hashed per chunk", ASSET_BYTES / 1024) {
	// **What shipping one asset actually costs**, which is neither of the rows
	// above on its own: cut it, then hash every piece. One iteration is a
	// kibibyte of asset, so this figure is the publishing pipeline's throughput
	// and it multiplies straight up to "how long does a build take".
	static const Chunker chunker;
	const std::vector<std::byte> &data = Bytes(ASSET_BYTES);

	const std::vector<ChunkSpan> spans = chunker.Split(data);
	uint8_t mixed = 0;
	for (const ChunkSpan &span : spans) {
		const std::span<const std::byte> piece(data.data() + span.Offset, span.Bytes);
		mixed ^= Hasher::Of(piece).Digest[0];
	}
	Consume(mixed);
	Consume(spans.size());
}

// --- the hash tree ------------------------------------------------------------
//
// One iteration is one **leaf**, so these rows are per-chunk and compare
// directly with the per-chunk hashing cost above. A 16 MiB asset at the default
// target is about 256 chunks; a whole game is hundreds of thousands.

BENCH("HashTree::Build · 256 leaves", 256) {
	const std::vector<ContentHash> &leaves = Leaves(256);
	Consume(HashTree::Build(leaves).Root().Digest[0]);
}

BENCH("HashTree::Build · 65k leaves", 65'536) {
	// A whole game's worth. **A flat per-leaf figure against the 256-leaf row is
	// the result to want** — a tree is linear in its leaves and logarithmic in
	// its depth, so per-leaf cost should not move. A climbing one means
	// something is being copied per level rather than combined.
	const std::vector<ContentHash> &leaves = Leaves(65'536);
	Consume(HashTree::Build(leaves).Root().Digest[0]);
}

BENCH("HashTree::RootOf · 65k leaves", 65'536) {
	// The root without keeping the tree, which is what a publisher wants when it
	// is not going to serve proofs from this build. Against `Build` at the same
	// size, the gap is what retaining the interior nodes costs.
	const std::vector<ContentHash> &leaves = Leaves(65'536);
	Consume(HashTree::RootOf(leaves).Digest[0]);
}

BENCH("HashTree::CombineNodes · 100k", 100'000) {
	// The primitive every level is made of. A tree over N leaves does N-1 of
	// these, so this row times the leaf count is the whole tree's cost — and if
	// it is not, `Build` is doing something besides combining.
	const std::vector<ContentHash> &leaves = Leaves(256);
	uint8_t mixed = 0;
	for (size_t index = 0; index < 100'000; index++) {
		mixed ^= HashTree::CombineNodes(leaves[index % 256], leaves[(index + 1) % 256]).Digest[0];
	}
	Consume(mixed);
}
