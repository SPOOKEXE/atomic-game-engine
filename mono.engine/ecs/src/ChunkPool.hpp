#pragma once

// The shared span pool: fixed-size column chunks, kept between the column that
// let one go and the column that asks next.
//
// **Why a pool rather than the allocator.** A chunked column gives a chunk back
// the moment its rows stop reaching into it — that release is the whole point
// of the item, because a world that peaked at ten thousand entities and settled
// at a hundred has to hand the difference back rather than hold it forever.
// Doing it straight to `operator new` would trade one problem for another: a
// population oscillating across a chunk boundary would allocate and free on
// every oscillation, and *more allocations of the same bytes* is exactly what
// got a smaller `SparseSet` page rejected — they interleave with the columns
// and scatter them, measured at 8% to 21% slower. The pool is what makes the
// release affordable, so it is not an optimisation beside the chunking; it is
// the half that stops the chunking from costing more than it saves.
//
// **Process-wide, not per store.** The shape this exists for is a thousand
// worlds in one host. A per-store pool would keep its spares a thousand times
// over, which relocates the leak rather than closing it — and worlds hand
// chunks around anyway, since one world's peak is another world's next
// allocation.
//
// **Retention is capped, because a pool that never trims reports success.**
// Past `RETAINED_BYTES_CAP` a released chunk goes back to the allocator, so
// what is held is bounded by a constant instead of by the high-water mark of
// whatever ran. `Trim` empties it outright, for a host that has just suspended
// a batch of worlds and wants the pages back now.
//
// Private to this module. A chunk is the storage layout, and `ecs/AGENTS.md`
// says the layout is the thing that must not become a migration cost.
//
// @tier L3 · shared

#include <cstddef>
#include <cstdint>

namespace engine::ecs {

	// A freelist of column chunks, keyed by byte size and alignment.
	//
	// @since v0.4
	class ChunkPool {
	  public:
		// Bytes the pool will hold in freed chunks before it starts handing them
		// back to the allocator.
		//
		// Eight mebibytes, and the number comes from the measurement this whole
		// item is filed against: a thousand worlds settled at a hundred entities
		// hold **2.7 MB of live rows**, so a host at that shape retains less in
		// spares than it holds in data. Large enough that a world oscillating
		// across a chunk boundary never reaches the allocator; small enough that
		// a host which genuinely shrank gives the pages back.
		static constexpr size_t RETAINED_BYTES_CAP = 8u * 1024u * 1024u;

		// Hands back a chunk of `bytes`, aligned to `alignment`.
		//
		// @param bytes     The chunk size, which must be non-zero.
		// @param alignment The alignment the component type requires.
		// @return The chunk, never null — an exhausted allocator throws.
		// @threadsafe
		static void *Acquire(size_t bytes, size_t alignment);

		// Takes a chunk back.
		//
		// `bytes` and `alignment` must be the ones it was acquired with: a sized
		// aligned `operator delete` is undefined otherwise, and the size class is
		// what decides who can be handed it next.
		//
		// @param chunk     The chunk, which may be null.
		// @param bytes     The size it was acquired with.
		// @param alignment The alignment it was acquired with.
		// @threadsafe
		static void Release(void *chunk, size_t bytes, size_t alignment);

		// Releases everything held, to the allocator.
		//
		// The explicit half of the trim policy, and the smaller half: the cap
		// above is what bounds a running host, and this is for one that knows it
		// has just stopped needing the spares. Nothing in the engine calls it
		// yet, deliberately — a hook into world suspension would buy at most
		// `RETAINED_BYTES_CAP` and would put a storage detail in an `ecs` public
		// header for `world` to reach. The day a host wants the pages back on a
		// schedule, this is the call it makes.
		//
		// @threadsafe
		static void Trim();

		// Bytes currently held in freed chunks.
		//
		// A diagnostic, and the one a test uses to prove the cap is honoured
		// rather than described.
		//
		// @return The retained bytes.
		// @threadsafe
		static size_t RetainedBytes();

		// Chunks taken from the allocator since the process started.
		//
		// **The number that says whether the pool is working.** A world cycling
		// across a chunk boundary should move this by nothing at all; if it
		// climbs, the pool is a freelist that never hits.
		//
		// @return The allocation count.
		// @threadsafe
		static uint64_t Allocations();

		// Chunks handed back out of the freelist since the process started.
		//
		// @return The reuse count.
		// @threadsafe
		static uint64_t Reuses();

	  private:
		ChunkPool() = delete;
	};
}
