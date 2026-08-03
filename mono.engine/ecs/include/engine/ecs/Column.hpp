#pragma once

// A run of fixed-size chunks holding component values whose type is known only
// at runtime.
//
// This is where a property physically lives. An archetype is a set of these —
// one per component in its set, all the same length and chunked on the same row
// boundaries — so a system iterating `<Transform, Motion>` walks two packed
// arrays a chunk at a time rather than chasing an object graph, and Luau writing
// `part.Size` resolves a name to a column and an offset.
//
// Four things follow from the shape, and all four are deliberate:
//
// - **Lifetime goes through the descriptor.** Constructing, destroying, copying
//   and moving are function pointers, because there is no `T` here to call.
//   For a trivially copyable component — nearly all of them — the column takes
//   a memcpy path and never calls one.
// - **Removal is swap-with-last.** Deleting a row touches two rows instead of
//   shifting a tail, which is what keeps destroying an entity O(1). Row order
//   is therefore not stable, and nothing may store a row index across a
//   removal.
// - **A row is reached through the chunk directory, never from one base
//   pointer.** There is no whole-column base address and there is deliberately
//   no accessor offering one: the invariant "one base pointer and a stride
//   reach every row" is the thing this layout gives up, and an API still
//   stating it is how a caller ends up walking off the end of chunk zero.
// - **Capacity follows the population down.** A chunk the rows no longer reach
//   into goes straight back to `ChunkPool`. That is the whole item: a world
//   that peaked at ten thousand entities and settled at a hundred used to hold
//   the peak forever, and a thousand such worlds in one host measured **703 MB
//   against 2.7 MB of live rows**. The pool is what makes giving it back
//   affordable — see `src/ChunkPool.hpp`, which carries that reasoning.
//
// A tag — a component with no data — is a column of zero-byte rows. It counts
// its rows and allocates nothing, because the presence of the component is the
// entire value.
//
// @tier L3 · shared

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>

#include <bit>
#include <cstddef>
#include <vector>

namespace engine::ecs {

	// A growable, type-erased array of one component type.
	//
	// Move-only. Copying one would need the caller to have decided whether the
	// components inside are copyable, and an archetype never wants to: it moves
	// rows between columns and hands out pointers.
	//
	// @since v0.2
	class Column {
	  public:
		// Rows in the first chunk, and the shift that says so.
		//
		// Eight, which is exactly the capacity a column used to jump to on its
		// first growth. That is the point: **nothing may get worse than it was**,
		// and the case that would have is the small one. A resource is a column of
		// a single row and some of them are hundreds of bytes wide, so a fixed
		// chunk of a thousand rows would have charged a world more than a megabyte
		// to hold one — worse than the leak this item exists to close, and worse
		// for every world rather than only the ones that shrank.
		static constexpr size_t FIRST_CHUNK_SHIFT = 3;

		// Rows in the first chunk, which is `1 << FIRST_CHUNK_SHIFT`.
		static constexpr size_t FIRST_CHUNK_ROWS = size_t{1} << FIRST_CHUNK_SHIFT;

		// The chunk one row falls in.
		//
		// **Chunks double, and that is what makes the layout pay on both axes.** A
		// fixed chunk size has to choose: small enough that a settled world gives
		// its peak back, or large enough that a big world's rows are not scattered
		// across hundreds of allocations. Measured, a fixed thousand rows cost
		// **8% on `Each` over 100k entities** — the same shape as the eight times
		// as many allocations that got a smaller `SparseSet` page rejected at
		// 8-21%.
		//
		// Doubling refuses the choice. Chunk zero holds eight rows and chunk `k`
		// holds `8 << (k-1)`, so capacity is never more than twice the rows: a
		// column of half a million rows is sixteen chunks with almost every row in
		// the largest two, and a column that settles at a hundred keeps a hundred
		// and twenty-eight. It is the growth curve the column already had, with
		// the copy taken out and the release put in.
		//
		// The boundaries are a function of the row index alone, so **every column
		// in an archetype divides identically** whatever its stride — which is what
		// `EachBatch<Transform, Motion>` needs to hand out two pointers over the
		// same rows.
		//
		// @param row The row index.
		// @return The chunk holding it.
		static size_t ChunkOf(size_t row) {
			// One `lzcnt`, because the curve is powers of two. This is the
			// division and modulo `ROADMAP.md` predicted chunking would cost.
			return static_cast<size_t>(std::bit_width(row >> FIRST_CHUNK_SHIFT));
		}

		// The first row a chunk holds.
		//
		// @param chunk The chunk index.
		// @return Its first row.
		static constexpr size_t ChunkStart(size_t chunk) {
			return chunk == 0 ? 0 : FIRST_CHUNK_ROWS << (chunk - 1);
		}

		// How many rows a chunk holds.
		//
		// @param chunk The chunk index.
		// @return Its row count.
		static constexpr size_t ChunkRows(size_t chunk) {
			return chunk == 0 ? FIRST_CHUNK_ROWS : FIRST_CHUNK_ROWS << (chunk - 1);
		}

		// One past the last row of the chunk `row` falls in.
		//
		// @param row The row index.
		// @return The first row of the next chunk.
		static size_t ChunkLimit(size_t row) {
			return FIRST_CHUNK_ROWS << ChunkOf(row);
		}

		// Chunks needed to hold `rows`.
		//
		// @param rows The row count.
		// @return The chunk count, zero for no rows.
		static size_t ChunksFor(size_t rows) {
			return rows == 0 ? 0 : ChunkOf(rows - 1) + 1;
		}

		// Creates a column with no type, holding nothing.
		//
		// Useful as a placeholder in a container; every operation other than
		// Size and Empty requires a type.
		Column() = default;

		// Creates an empty column for one component type.
		//
		// @param type The component whose values this column holds.
		explicit Column(ComponentId type);

		// Destroys every row and releases the storage.
		~Column();

		// Takes over another column's storage, leaving it empty and typeless.
		Column(Column &&other) noexcept;

		// Takes over another column's storage, destroying what this one held.
		Column &operator=(Column &&other) noexcept;

		// A column owns its rows and is not copied.
		Column(const Column &) = delete;

		// A column owns its rows and is not copy-assigned.
		Column &operator=(const Column &) = delete;

		// The component type this column holds.
		//
		// @return The type, or an invalid id for a default-constructed column.
		ComponentId Type() const {
			return ComponentType;
		}

		// The type information behind this column.
		//
		// @return The descriptor, or an empty one for a typeless column.
		const TypeDescriptor &Describe() const;

		// The number of rows.
		//
		// @return The current row count.
		size_t Size() const {
			return Rows;
		}

		// The number of rows that fit without reallocating.
		//
		// @return The current capacity in rows.
		size_t Capacity() const {
			return Capacity_;
		}

		// Reports whether the column holds no rows.
		//
		// @return `true` when there are no rows.
		bool Empty() const {
			return Rows == 0;
		}

		// Bytes of storage this column is holding, live rows or not.
		//
		// A diagnostic, and it exists for the same reason
		// `SparseSet::ResidentSlots` does: a world that peaked at ten thousand
		// entities and settled at a hundred keeps the peak, and a fee that is
		// only described in a comment is one that grows back. Zero for a tag,
		// which owns no memory whatever its row count says.
		//
		// @return The resident bytes.
		size_t ResidentBytes() const {
			return Capacity_ * Stride;
		}

		// The chunk directory: one base address per chunk, in row order.
		//
		// What the iteration paths resolve once per table and then index by
		// `row >> CHUNK_SHIFT`. Empty — and therefore `nullptr` — for a tag
		// column, for a typeless one, and for a data column holding no rows,
		// which is why every caller checks the row count first rather than the
		// pointer.
		//
		// Invalidated by anything that changes the chunk count, which is any
		// push past a boundary and any removal back across one.
		//
		// @return The directory, or `nullptr` when there are no chunks.
		void *const *ChunkData() const {
			return Chunks.data();
		}

		// How many chunks the column is holding.
		//
		// @return The chunk count, zero for a tag column.
		size_t ChunkCount() const {
			return Chunks.size();
		}

		// One row's address.
		//
		// Bounds are the caller's responsibility on the hot path, so this does
		// not check them; the archetype above is what knows a row is valid.
		//
		// @param row The row index, which must be less than Size().
		// @return The value's address, or `nullptr` for a tag column.
		void *At(size_t row) {
			if (Chunks.empty()) {
				return nullptr;
			}
			const size_t chunk = ChunkOf(row);
			return static_cast<std::byte *>(Chunks[chunk]) + (row - ChunkStart(chunk)) * Stride;
		}

		// One row's address, read-only.
		//
		// @param row The row index, which must be less than Size().
		// @return The value's address, or `nullptr` for a tag column.
		const void *At(size_t row) const {
			return const_cast<Column *>(this)->At(row);
		}

		// Grows the capacity to at least `rows`, never shrinking it.
		//
		// Rounded up to a whole chunk, and the chunks come from the pool. It is
		// an optimisation and not a promise about addresses: a later removal
		// hands the chunks past the row count straight back, which is what makes
		// the settled world stop paying for its peak.
		//
		// @param rows The row capacity to guarantee.
		void Reserve(size_t rows);

		// Destroys every row and gives every chunk back to the pool.
		void Clear();

		// Appends one default-constructed row.
		//
		// @return The index of the new row.
		size_t PushDefault();

		// Appends one row copy-constructed from `value`.
		//
		// @param value The value to copy, ignored for a tag column.
		// @return The index of the new row.
		size_t PushCopy(const void *value);

		// Overwrites one row by copy-assignment from `value`.
		//
		// @param row   The row to overwrite, which must be less than Size().
		// @param value The value to copy, ignored for a tag column.
		void Assign(size_t row, const void *value);

		// Removes one row by moving the last row into its place.
		//
		// O(1), and the reason row order is not stable. Removing the last row
		// is just a destroy.
		//
		// @param row The row to remove, which must be less than Size().
		void RemoveSwapBack(size_t row);

		// Appends one row moved out of another column of the same type.
		//
		// The source row is left constructed and destructible; the caller
		// removes it. This is the archetype-move path — an entity gaining a
		// component moves each of its existing values to the new archetype's
		// columns rather than copying them.
		//
		// @param source    The column to move from, which must hold the same type.
		// @param sourceRow The row to move.
		// @return The index of the new row.
		size_t PushMovedFrom(Column &source, size_t sourceRow);

		// Appends one row copy-constructed from another column of the same type.
		//
		// @param source    The column to copy from, which must hold the same type.
		// @param sourceRow The row to copy.
		// @return The index of the new row.
		size_t PushCopiedFrom(const Column &source, size_t sourceRow);

		// Appends every row to a writer.
		//
		// Writes nothing and marks nothing for a type with no serialisation;
		// the caller checks `Describe().Serialisable` first, because a snapshot
		// that silently skipped a column would restore a world missing state.
		//
		// @param writer The writer to append to.
		// @return `true` when the column was written.
		bool Write(core::ByteWriter &writer) const;

		// Replaces the contents with `rows` values read from `reader`.
		//
		// On a short or corrupt buffer the reader is left failed and the column
		// is left empty rather than half-populated.
		//
		// @param reader The reader to consume.
		// @param rows   The number of rows the caller expects.
		// @return `true` when every row was read.
		bool Read(core::ByteReader &reader, size_t rows);

	  private:
		// Makes sure a chunk exists for the row about to be appended.
		void GrowIfFull();

		// Takes chunks from the pool until there are `chunks` of them.
		void GrowToChunks(size_t chunks);

		// Hands every chunk past `chunks` back to the pool.
		//
		// The rows in them must already have been destroyed.
		void ReleaseChunksFrom(size_t chunks);

		// Destroys the rows in `[from, to)`, one contiguous run per chunk.
		void DestroyRows(size_t from, size_t to);

		// The bytes one chunk of this column occupies.
		size_t ChunkBytes(size_t chunk) const {
			return ChunkRows(chunk) * Stride;
		}

		ComponentId ComponentType;

		// Cached from the descriptor. A column is walked per row on the hot
		// path, and reaching through the registry for a size every time would
		// put a lock and a lookup inside the loop.
		size_t Stride = 0;
		size_t Alignment = 1;
		bool Trivial = false;

		// One base address per chunk. Never reallocated in place — a chunk keeps
		// its address for its whole time in this column, so only the directory
		// moves when the count changes.
		std::vector<void *> Chunks;

		size_t Rows = 0;
		size_t Capacity_ = 0;
	};
}
