#include <engine/assets/Chunker.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.assets.chunker")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::assets::Chunker;
using engine::assets::ChunkLimits;
using engine::assets::ChunkSpan;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	// Deterministic pseudo-random bytes. A fixed generator rather than
	// core::Random because these tests are about boundaries being a function of
	// content, and a shared generator whose sequence somebody later tunes would
	// move every boundary here without touching this file.
	std::vector<std::byte> Content(size_t length, uint64_t seed) {
		std::vector<std::byte> data(length);
		uint64_t state = seed;
		for (std::byte &value : data) {
			state += 0x9E3779B97F4A7C15ull;
			uint64_t mixed = state;
			mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
			mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
			mixed ^= mixed >> 31;
			value = static_cast<std::byte>(mixed & 0xFF);
		}
		return data;
	}

	// Small limits, so a test can produce many boundaries out of a few hundred
	// kilobytes instead of tens of megabytes.
	ChunkLimits Small() {
		return ChunkLimits{256, 1024, 4096};
	}

	bool Tiles(const std::vector<ChunkSpan> &chunks, size_t total) {
		uint64_t expected = 0;
		for (const ChunkSpan &chunk : chunks) {
			if (chunk.Offset != expected || chunk.Bytes == 0) {
				return false;
			}
			expected += chunk.Bytes;
		}
		return expected == total;
	}
}

TEST_CASE("chunks tile the input exactly", "[assets][chunker]") {
	const auto data = Content(200 * 1024, 1);
	const Chunker chunker(Small());

	const auto chunks = chunker.Split(data);

	REQUIRE_FALSE(chunks.empty());
	CHECK(Tiles(chunks, data.size()));
}

TEST_CASE("every chunk respects the size envelope", "[assets][chunker]") {
	const auto data = Content(200 * 1024, 2);
	const ChunkLimits limits = Small();
	const Chunker chunker(limits);

	const auto chunks = chunker.Split(data);
	REQUIRE(chunks.size() > 4);

	for (size_t index = 0; index < chunks.size(); ++index) {
		CHECK(chunks[index].Bytes <= limits.MaximumBytes);
		// The last chunk is whatever is left and may be short; every other one
		// was cut by the algorithm and must clear the floor.
		if (index + 1 < chunks.size()) {
			CHECK(chunks[index].Bytes >= limits.MinimumBytes);
		}
	}
}

TEST_CASE("the same bytes always cut the same way", "[assets][chunker]") {
	const auto data = Content(200 * 1024, 3);
	const Chunker first(Small());
	const Chunker second(Small());

	// The requirement behind dedup: two peers that chunk differently share
	// nothing, and nothing anywhere reports that they have stopped sharing.
	const auto left = first.Split(data);
	const auto right = second.Split(data);

	REQUIRE(left.size() == right.size());
	for (size_t index = 0; index < left.size(); ++index) {
		CHECK(left[index].Offset == right[index].Offset);
		CHECK(left[index].Bytes == right[index].Bytes);
	}
}

TEST_CASE("an insertion moves only the chunks around it", "[assets][chunker]") {
	const auto original = Content(400 * 1024, 4);
	const Chunker chunker(Small());
	const auto before = chunker.Split(original);

	// One byte inserted near the front. With fixed-size chunking every later
	// boundary shifts and every later chunk is new — which is the entire reason
	// this class exists rather than a divide by 64 KiB.
	std::vector<std::byte> edited;
	edited.reserve(original.size() + 1);
	edited.insert(edited.end(), original.begin(), original.begin() + 5000);
	edited.push_back(std::byte{0x7F});
	edited.insert(edited.end(), original.begin() + 5000, original.end());

	const auto after = chunker.Split(edited);

	// Count the chunks whose content survived. Compare by bytes rather than by
	// offset, because every offset after the insertion has moved by one and
	// that is not what "the same chunk" means.
	const auto contentOf = [](const std::vector<std::byte> &source, const ChunkSpan &chunk) {
		return std::vector<std::byte>(
			source.begin() + static_cast<ptrdiff_t>(chunk.Offset),
			source.begin() + static_cast<ptrdiff_t>(chunk.Offset) + chunk.Bytes
		);
	};

	std::vector<std::vector<std::byte>> oldChunks;
	for (const ChunkSpan &chunk : before) {
		oldChunks.push_back(contentOf(original, chunk));
	}

	size_t shared = 0;
	for (const ChunkSpan &chunk : after) {
		const auto candidate = contentOf(edited, chunk);
		if (std::find(oldChunks.begin(), oldChunks.end(), candidate) != oldChunks.end()) {
			++shared;
		}
	}

	INFO("shared " << shared << " of " << after.size());
	// Loose on purpose. The exact figure depends on the mask and the content,
	// and pinning it would make this test a change-detector for a constant
	// CDN.md §9 says is not measured yet. Half is far above what fixed-size
	// chunking manages here, which is near zero.
	CHECK(shared > after.size() / 2);
}

TEST_CASE("an empty input produces no chunks", "[assets][chunker]") {
	const Chunker chunker(Small());

	// Not one empty chunk. A zero-length chunk is a hash, a manifest row and a
	// fetch that transfers nothing.
	CHECK(chunker.Split({}).empty());
	CHECK(chunker.NextBoundary({}) == 0);
}

TEST_CASE("an input below the minimum is one chunk", "[assets][chunker]") {
	const auto data = Content(100, 5);
	const Chunker chunker(Small());

	const auto chunks = chunker.Split(data);
	REQUIRE(chunks.size() == 1);
	CHECK(chunks[0].Offset == 0);
	CHECK(chunks[0].Bytes == data.size());
}

TEST_CASE("a boundary is forced at the maximum", "[assets][chunker]") {
	const ChunkLimits limits{16, 32, 64};
	const Chunker chunker(limits);

	// Content with no variety at all never satisfies the mask, so every cut
	// here is the forced one. A chunker without that ceiling returns the whole
	// input as one chunk and nothing bounds a group.
	const std::vector<std::byte> flat(1000, std::byte{0});
	const auto chunks = chunker.Split(flat);

	REQUIRE(chunks.size() > 1);
	CHECK(Tiles(chunks, flat.size()));
	for (const ChunkSpan &chunk : chunks) {
		CHECK(chunk.Bytes <= limits.MaximumBytes);
	}
}

TEST_CASE("invalid limits fall back to the defaults", "[assets][chunker]") {
	// A bad envelope is a configuration mistake. Refusing to chunk would turn
	// it into a failure a long way from its cause.
	const Chunker inverted(ChunkLimits{4096, 1024, 256});
	CHECK(inverted.Limits().MinimumBytes == ChunkLimits{}.MinimumBytes);
	CHECK(inverted.Limits().TargetBytes == ChunkLimits{}.TargetBytes);

	const Chunker zeroed(ChunkLimits{0, 0, 0});
	CHECK(zeroed.Limits().MinimumBytes == ChunkLimits{}.MinimumBytes);

	CHECK(ChunkLimits{}.IsValid());
	CHECK_FALSE(ChunkLimits{4096, 1024, 256}.IsValid());
	CHECK_FALSE(ChunkLimits{0, 1024, 4096}.IsValid());
}

TEST_CASE("NextBoundary agrees with Split", "[assets][chunker]") {
	const auto data = Content(100 * 1024, 6);
	const Chunker chunker(Small());
	const auto chunks = chunker.Split(data);

	// The streaming caller cuts one chunk at a time and must land on exactly
	// the same boundaries as the whole-buffer path, or a file chunked by a
	// stream dedups against nothing chunked in memory.
	size_t offset = 0;
	for (const ChunkSpan &chunk : chunks) {
		const size_t length = chunker.NextBoundary(std::span<const std::byte>(data).subspan(offset));
		CHECK(length == chunk.Bytes);
		offset += length;
	}
	CHECK(offset == data.size());
}

TEST_CASE("the average chunk lands near the target", "[assets][chunker]") {
	const auto data = Content(2 * 1024 * 1024, 7);
	const ChunkLimits limits = Small();
	const Chunker chunker(limits);

	const auto chunks = chunker.Split(data);
	REQUIRE(chunks.size() > 100);

	const double average = static_cast<double>(data.size()) / static_cast<double>(chunks.size());
	INFO("average " << average << " target " << limits.TargetBytes);

	// A factor of two either way. This is what the normalised two-mask scheme
	// buys — a single mask gives a geometric distribution whose average drifts
	// much further than this once the floor and ceiling clamp it.
	CHECK(average > static_cast<double>(limits.TargetBytes) / 2.0);
	CHECK(average < static_cast<double>(limits.TargetBytes) * 2.0);
}

TEST_CASE(
	"splitting reports itself to the frame graph and the metrics sink", "[assets][chunker][framegraph]"
) {
	const auto data = Content(64 * 1024, 8);
	const Chunker chunker(Small());

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	const auto chunks = chunker.Split(data);
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
		return span.Name == "Chunker::Split";
	}));

	// One span per asset and the detail in counters, rather than a span per
	// chunk — a large asset cuts into thousands and the graph holds 4096 spans
	// in total.
	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	CHECK(total("assets.chunks.cut") == static_cast<double>(chunks.size()));
	CHECK(total("assets.chunks.bytes") == static_cast<double>(data.size()));
}

TEST_CASE("the warm-up window does not move a boundary", "[assets][chunker]") {
	// **This is a format test wearing a performance test's clothes.**
	//
	// `NextBoundary` used to feed its rolling hash from byte zero up to
	// `MinimumBytes` before testing anything, and now feeds it from only 64
	// bytes before. The justification is arithmetic: the hash is
	// `(hash << 1) + GEAR[byte]`, so a byte 64 positions back has been shifted
	// out of a 64-bit register entirely and cannot affect any tested position.
	//
	// If that arithmetic is wrong, nothing crashes and no other test fails. What
	// happens instead is that every chunk boundary moves — so a client and an
	// origin on different builds share no chunks, every fetch is a full
	// download, and the dedup the whole content system is built on silently
	// stops working. That failure is invisible from inside one build, which is
	// exactly why it needs pinning against an independent implementation rather
	// than against a recorded number somebody could regenerate.
	//
	// The reference below is the old walk, written out in full: same table, same
	// masks, warmed from zero. It has to agree on every boundary of a body of
	// content large enough to contain thousands of them.

	// The gear table, rebuilt here rather than shared. A test that imported the
	// production constant would still pass if the constant itself were the
	// thing that changed.
	std::vector<uint64_t> gear(256);
	{
		uint64_t state = 0x9E3779B97F4A7C15ull;
		for (uint64_t &entry : gear) {
			state += 0x9E3779B97F4A7C15ull;
			uint64_t mixed = state;
			mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
			mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
			entry = mixed ^ (mixed >> 31);
		}
	}

	const auto spreadMask = [](int bits) {
		uint64_t mask = 0;
		for (int index = 0; index < bits; ++index) {
			mask |= uint64_t{1} << (63 - index * 3);
		}
		return mask;
	};

	const ChunkLimits limits = Small();

	// The same derivation the constructor runs: log2 of the target, plus and
	// minus two, clamped to 21.
	const auto clamp = [](int bits) { return bits < 1 ? 1 : (bits > 21 ? 21 : bits); };
	const int target = clamp(static_cast<int>(std::bit_width(limits.TargetBytes) - 1));
	const uint64_t strict = spreadMask(clamp(target + 2));
	const uint64_t loose = spreadMask(clamp(target - 2));

	// The old walk: warmed from byte zero.
	const auto fromZero = [&](std::span<const std::byte> data) -> size_t {
		if (data.empty()) {
			return 0;
		}
		const size_t available = data.size();
		const size_t minimum = std::min(limits.MinimumBytes, available);
		const size_t targetBytes = std::min(limits.TargetBytes, available);
		const size_t maximum = std::min(limits.MaximumBytes, available);

		uint64_t hash = 0;
		size_t index = 0;
		for (; index < minimum; ++index) {
			hash = (hash << 1) + gear[static_cast<uint8_t>(data[index])];
		}
		for (; index < targetBytes; ++index) {
			hash = (hash << 1) + gear[static_cast<uint8_t>(data[index])];
			if ((hash & strict) == 0) {
				return index + 1;
			}
		}
		for (; index < maximum; ++index) {
			hash = (hash << 1) + gear[static_cast<uint8_t>(data[index])];
			if ((hash & loose) == 0) {
				return index + 1;
			}
		}
		return maximum;
	};

	const Chunker chunker(limits);
	const std::vector<std::byte> data = Content(2 * 1024 * 1024, 0x5EED'1234ull);

	// Every boundary, not merely the first: an error that only bites once the
	// window has been fed a particular way would show up somewhere in the middle
	// rather than at the start.
	size_t offset = 0;
	size_t compared = 0;
	while (offset < data.size()) {
		const std::span<const std::byte> rest(data.data() + offset, data.size() - offset);
		const size_t reference = fromZero(rest);
		const size_t produced = chunker.NextBoundary(rest);
		REQUIRE(produced == reference);
		offset += produced;
		compared++;
	}

	// A body of content this size at these limits cuts into thousands. Asserted
	// so that a future change making `NextBoundary` return the whole input in
	// one piece cannot pass this test by comparing one boundary against itself.
	CHECK(compared > 500);

	// The degenerate stream too, where no boundary is ever found and every chunk
	// is forced at the maximum — a different path through the same function, and
	// the one where the warm-up is the largest share of the work.
	const std::vector<std::byte> zeroes(64 * 1024);
	offset = 0;
	while (offset < zeroes.size()) {
		const std::span<const std::byte> rest(zeroes.data() + offset, zeroes.size() - offset);
		const size_t produced = chunker.NextBoundary(rest);
		REQUIRE(produced == fromZero(rest));
		offset += produced;
	}
}
