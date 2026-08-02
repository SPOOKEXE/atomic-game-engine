#include <engine/assets/Chunker.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <array>
#include <bit>

namespace engine::assets {

	namespace {
		// SplitMix64. Used only to generate the gear table below, never at run
		// time — it is here so that 256 frozen constants can be read as an
		// algorithm and a seed rather than as a wall of hex nobody can check.
		constexpr uint64_t SplitMix64(uint64_t &state) {
			state += 0x9E3779B97F4A7C15ull;
			uint64_t mixed = state;
			mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
			mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
			return mixed ^ (mixed >> 31);
		}

		// The seed is the golden-ratio constant and carries no other meaning.
		// It is written down because "some fixed number" and "this fixed
		// number" are different claims, and only the second one is reproducible.
		constexpr uint64_t GEAR_SEED = 0x9E3779B97F4A7C15ull;

		// One random 64-bit value per byte value. The rolling hash is
		//
		//     hash = (hash << 1) + Gear[byte]
		//
		// so the hash after any point depends on the last 64 bytes, with the
		// most recent bytes reaching the low bits and older ones shifted up.
		//
		// Frozen forever. This table is the format.
		constexpr std::array<uint64_t, 256> BuildGearTable() {
			std::array<uint64_t, 256> table{};
			uint64_t state = GEAR_SEED;
			for (auto &entry : table) {
				entry = SplitMix64(state);
			}
			return table;
		}

		constexpr std::array<uint64_t, 256> GEAR = BuildGearTable();

		// A mask with `bits` bits set, spread across the high end.
		//
		// Two decisions in one small function, and both matter.
		//
		// **The bit count sets the average.** A boundary is taken when the
		// masked bits are all zero, so the expected run is 2^bits bytes.
		//
		// **The bits are spread rather than contiguous, and taken from the top.**
		// With `hash = (hash << 1) + Gear[byte]`, bit *n* of the hash has been
		// shifted through *n* times and therefore mixes *n* bytes of history —
		// so the low bits depend on almost nothing and a mask taking them would
		// cut on a far shorter window than its bit count implies. Adjacent bits
		// are also correlated, one being the previous byte's shifted neighbour,
		// which is why this steps by three rather than one.
		constexpr uint64_t SpreadMask(int bits) {
			uint64_t mask = 0;
			for (int index = 0; index < bits; ++index) {
				mask |= uint64_t{1} << (63 - index * 3);
			}
			return mask;
		}

		// How many mask bits a target size wants: log2, rounded down.
		int BitsForSize(size_t bytes) {
			if (bytes < 2) {
				return 1;
			}
			return static_cast<int>(std::bit_width(bytes) - 1);
		}

		// Bits are stepped by three and there are 64 of them, so a mask cannot
		// ask for more than this many without running off the bottom.
		constexpr int MAXIMUM_MASK_BITS = 21;

		int ClampBits(int bits) {
			if (bits < 1) {
				return 1;
			}
			return bits > MAXIMUM_MASK_BITS ? MAXIMUM_MASK_BITS : bits;
		}
	}

	bool ChunkLimits::IsValid() const {
		return MinimumBytes > 0 && MinimumBytes <= TargetBytes && TargetBytes <= MaximumBytes;
	}

	Chunker::Chunker(ChunkLimits limits) : Envelope(limits.IsValid() ? limits : ChunkLimits{}) {
		// Normalised chunking, from FastCDC. One mask produces a size
		// distribution far wider than its average suggests — a geometric one,
		// so short chunks are the most likely single outcome even when the mean
		// is 64 KiB. Two masks pull it in: below the target, a *stricter* mask
		// makes an early cut less likely; above it, a *looser* one makes a late
		// cut more likely. The average is unchanged and the spread is much
		// tighter, which is what makes a group's compressed size predictable
		// enough to bound — CDN.md §5.
		const int target = ClampBits(BitsForSize(Envelope.TargetBytes));
		StrictMask = SpreadMask(ClampBits(target + 2));
		LooseMask = SpreadMask(ClampBits(target - 2));
	}

	size_t Chunker::NextBoundary(std::span<const std::byte> data) const {
		if (data.empty()) {
			return 0;
		}

		// Everything below is in terms of these three, clamped to what is
		// actually left. Without the clamp the minimum could exceed the input
		// and the scan would start past the end.
		const size_t available = data.size();
		const size_t minimum = Envelope.MinimumBytes < available ? Envelope.MinimumBytes : available;
		const size_t target = Envelope.TargetBytes < available ? Envelope.TargetBytes : available;
		const size_t maximum = Envelope.MaximumBytes < available ? Envelope.MaximumBytes : available;

		// The window has to have run over the bytes before the minimum for the
		// hash at the minimum to mean anything, so it is fed from zero and only
		// *tested* from the minimum onward. Skipping the bytes entirely — which
		// some implementations do for speed — makes the boundary depend on where
		// the previous chunk happened to end, and boundaries that depend on
		// history do not dedup.
		uint64_t hash = 0;
		size_t index = 0;

		for (; index < minimum; ++index) {
			hash = (hash << 1) + GEAR[static_cast<uint8_t>(data[index])];
		}

		for (; index < target; ++index) {
			hash = (hash << 1) + GEAR[static_cast<uint8_t>(data[index])];
			if ((hash & StrictMask) == 0) {
				return index + 1;
			}
		}

		for (; index < maximum; ++index) {
			hash = (hash << 1) + GEAR[static_cast<uint8_t>(data[index])];
			if ((hash & LooseMask) == 0) {
				return index + 1;
			}
		}

		// No boundary the content asked for. Cut anyway — a chunk nothing
		// bounds is a chunk nothing can stream, cache or bound a group by.
		return maximum;
	}

	std::vector<ChunkSpan> Chunker::Split(std::span<const std::byte> data) const {
		// One span per asset rather than per chunk. A per-chunk span is the
		// granularity somebody reaches for first and it is wrong here: a large
		// asset cuts into thousands, and the frame graph holds 4096 spans in
		// total. The counters below carry the per-chunk detail instead, which
		// is an atomic add rather than a tree node.
		ENGINE_PROFILE("Chunker::Split");

		std::vector<ChunkSpan> chunks;
		if (data.empty()) {
			return chunks;
		}

		// One row per chunk, and the count is known within a factor of a few.
		// Reserving against the target rather than the minimum keeps the guess
		// from being several times too large on ordinary content.
		chunks.reserve(data.size() / Envelope.TargetBytes + 1);

		uint64_t offset = 0;
		while (offset < data.size()) {
			const size_t length = NextBoundary(data.subspan(offset));
			if (length == 0) {
				break;
			}
			chunks.push_back(ChunkSpan{offset, static_cast<uint32_t>(length)});
			offset += length;
		}

		// Both, because either alone is misleading: a chunk count says nothing
		// about size and a byte total says nothing about how finely it was cut,
		// and the ratio between them is the number that says whether the mask
		// is doing what CDN.md §9 assumes it does.
		core::Metrics::Count("assets.chunks.cut", static_cast<double>(chunks.size()));
		core::Metrics::Count("assets.chunks.bytes", static_cast<double>(data.size()));

		return chunks;
	}
}
