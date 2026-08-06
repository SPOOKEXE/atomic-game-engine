#pragma once

// Content-defined chunking. The rolling hash chooses boundaries; BLAKE3 names
// the resulting bytes. The gear table and seed are format constants.
// @tier L8 · shared

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::assets {

	// Size envelope for chunking.
	struct ChunkLimits {
		// No boundary is taken before this many bytes. Small chunks cost more
		// in per-chunk hashing and manifest rows than they save in dedup.
		size_t MinimumBytes = 16 * 1024;

		// The size the mask is tuned to produce on average.
		size_t TargetBytes = 64 * 1024;

		// A boundary is forced here whether or not the content asks for one, so
		// that one pathological stretch cannot produce a chunk nothing can
		// stream or cache.
		size_t MaximumBytes = 256 * 1024;

		// Whether these three can be used. Requires 0 < Minimum <= Target <=
		// Maximum; anything else would make the walk below either loop or skip.
		bool IsValid() const;
	};

	// One chunk's position within the stream it was cut from.
	struct ChunkSpan {
		// Byte offset from the start of the stream.
		uint64_t Offset = 0;

		// Length in bytes. Never zero, and never above ChunkLimits::MaximumBytes.
		uint32_t Bytes = 0;
	};

	// Cuts a byte stream into content-defined chunks.
	//
	// Stateless with respect to the data: the same bytes and the same limits
	// give the same boundaries on any machine, in any build, forever. That is a
	// requirement rather than a nicety — two peers that chunk differently share
	// nothing, and the dedup this exists for silently stops working.
	class Chunker {
	  public:
		// @param limits The size envelope. Invalid limits fall back to the
		//        defaults rather than aborting, because a bad envelope is a
		//        configuration mistake and refusing to chunk at all would turn
		//        it into a failure a long way from its cause.
		explicit Chunker(ChunkLimits limits = {});

		// The limits actually in use, after the validity fallback.
		const ChunkLimits &Limits() const {
			return Envelope;
		}

		// Cuts a whole buffer.
		//
		// The spans tile the input exactly: they are contiguous, in order, and
		// their lengths sum to `data.size()`. An empty input gives no spans
		// rather than one empty span — a zero-length chunk has a hash and no
		// content, which is a row in every manifest that references it and a
		// fetch that transfers nothing.
		//
		// @param data The bytes to cut.
		// @return The chunks, in stream order.
		std::vector<ChunkSpan> Split(std::span<const std::byte> data) const;

		// The length of the first chunk of `data`, which must start on a
		// boundary.
		//
		// The primitive Split is written in terms of, exposed because a caller
		// streaming a file cuts it one chunk at a time without holding the whole
		// thing.
		//
		// @param data Bytes from a chunk boundary onward.
		// @return The chunk length, or zero when `data` is empty. Never more
		//         than ChunkLimits::MaximumBytes.
		size_t NextBoundary(std::span<const std::byte> data) const;

	  private:
		ChunkLimits Envelope;

		// Masks derived from the limits at construction. Strict applies before
		// the target and loose after it — see the source for why one mask alone
		// gives a much wider size spread than its average suggests.
		uint64_t StrictMask = 0;
		uint64_t LooseMask = 0;
	};
}
