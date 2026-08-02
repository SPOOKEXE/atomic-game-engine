#pragma once

// Where one chunk ends and the next begins.
//
// Boundaries are decided by the *content* rather than by a byte count, using a
// rolling hash over a short window. That is the whole reason dedup works: with
// fixed-size chunks, inserting one byte near the front of a file shifts every
// later boundary and makes every later chunk new. Content-defined boundaries
// move with the content, so an insertion changes the chunks around it and
// nothing else. DATATYPES_LIBRARIES.md §1.1 names the class — rolling, gear.
//
// **The gear table and the mask construction below are part of the format.**
// Change either and every chunk boundary in the world moves, which means every
// stored chunk, every manifest and every client's cache is invalidated at once.
// They are generated from a stated seed rather than pasted as 256 magic numbers
// so that the thing being frozen is an algorithm somebody can review.
//
// This is *not* a hash anybody trusts. It decides where to cut and nothing
// else; the address of the resulting chunk is BLAKE3 over its bytes. A rolling
// hash is trivially collidable and must never name content — ContentHash.hpp.
//
// @tier L8 · shared

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::assets {

	// The size envelope a chunker cuts within.
	//
	// The defaults are a starting point rather than a measurement — CDN.md §9
	// carries that as an open question, and AGENTS.md asks for a number in a
	// comment beside an algorithm choice. There is not one here yet, and saying
	// so is better than implying the values were derived.
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
