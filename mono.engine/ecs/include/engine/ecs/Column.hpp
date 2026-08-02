#pragma once

// One contiguous array of component values whose type is known only at runtime.
//
// This is where a property physically lives. An archetype is a set of these —
// one per component in its set, all the same length — so a system iterating
// `<Transform, Motion>` walks two packed arrays rather than chasing an object
// graph, and Luau writing `part.Size` resolves a name to a column and an offset.
//
// Three things follow from being type-erased, and all three are deliberate:
//
// - **Lifetime goes through the descriptor.** Constructing, destroying, copying
//   and moving are function pointers, because there is no `T` here to call.
//   For a trivially copyable component — nearly all of them — the column takes
//   a memcpy path and never calls one.
// - **Removal is swap-with-last.** Deleting a row touches two rows instead of
//   shifting a tail, which is what keeps destroying an entity O(1). Row order
//   is therefore not stable, and nothing may store a row index across a
//   removal.
// - **Capacity is never given back.** A column that reached ten thousand rows
//   stays able to hold ten thousand, because a world whose population
//   oscillates would otherwise reallocate on every oscillation.
//
// A tag — a component with no data — is a column of zero-byte rows. It counts
// its rows and allocates nothing, because the presence of the component is the
// entire value.
//
// @tier L3 · shared

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>

#include <cstddef>

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

		// The first row's address, for a caller iterating the whole array.
		//
		// Invalidated by anything that grows the column. Null for a tag column
		// and for a typeless one, which is why a batch iterator has to take the
		// row count as the authority rather than the pointer.
		//
		// @return The base address, or `nullptr` when there is nothing to point at.
		void *Data() {
			return Storage;
		}

		// The first row's address, read-only.
		//
		// @return The base address, or `nullptr` when there is nothing to point at.
		const void *Data() const {
			return Storage;
		}

		// One row's address.
		//
		// Bounds are the caller's responsibility on the hot path, so this does
		// not check them; the archetype above is what knows a row is valid.
		//
		// @param row The row index, which must be less than Size().
		// @return The value's address, or `nullptr` for a tag column.
		void *At(size_t row) {
			return Storage == nullptr ? nullptr : static_cast<std::byte *>(Storage) + row * Stride;
		}

		// One row's address, read-only.
		//
		// @param row The row index, which must be less than Size().
		// @return The value's address, or `nullptr` for a tag column.
		const void *At(size_t row) const {
			return Storage == nullptr ? nullptr : static_cast<const std::byte *>(Storage) + row * Stride;
		}

		// Grows the capacity to at least `rows`, never shrinking it.
		//
		// @param rows The row capacity to guarantee.
		void Reserve(size_t rows);

		// Destroys every row and keeps the capacity.
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
		// Grows to hold at least one more row.
		void GrowIfFull();

		// Allocates `rows` worth of storage and moves the existing rows into it.
		void Reallocate(size_t rows);

		// Releases the storage without touching the rows, which must already
		// have been destroyed.
		void Release();

		ComponentId ComponentType;

		// Cached from the descriptor. A column is walked per row on the hot
		// path, and reaching through the registry for a size every time would
		// put a lock and a lookup inside the loop.
		size_t Stride = 0;
		size_t Alignment = 1;
		bool Trivial = false;

		void *Storage = nullptr;
		size_t Rows = 0;
		size_t Capacity_ = 0;
	};
}
