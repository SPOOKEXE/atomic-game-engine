#pragma once

// One table: every entity carrying exactly one set of components.
//
// Private to this module on purpose. An archetype is the storage layout, and
// the layout is the thing most likely to change — `ecs/AGENTS.md` says
// everything public here is a migration cost, and a table is not something
// userland or another module should be able to name. `Store` exposes what an
// archetype can do, never the archetype.
//
// The shape is one entity id array plus one Column per component in the set,
// all the same length, so row `n` of every column belongs to entity `n` of the
// id array. A system iterating `<Transform, Motion>` gets two base pointers and
// a count.
//
// Rows are removed by swapping the last one into the hole, so **row indices are
// not stable** and every removal tells the caller which entity moved, because
// that entity's directory entry now points at the wrong row.
//
// @tier L3 · shared

#include <engine/ecs/Column.hpp>
#include <engine/ecs/ComponentSet.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <vector>

namespace engine::ecs {

	// The rows of every entity sharing one component set.
	class Archetype {
	  public:
		// Creates an empty table for one component set.
		//
		// @param set The interned set this table holds, which outlives it.
		explicit Archetype(const ComponentSet &set);

		// A table is built in place and never copied.
		Archetype(const Archetype &) = delete;

		// A table is built in place and never assigned.
		Archetype &operator=(const Archetype &) = delete;

		// Tables move so that the store can hold them in a growable container.
		Archetype(Archetype &&) noexcept = default;

		// Tables move so that the store can hold them in a growable container.
		Archetype &operator=(Archetype &&) noexcept = default;

		// The component set this table holds.
		//
		// @return The interned set.
		const ComponentSet &Set() const {
			return *Members;
		}

		// The number of rows, which is the number of entities in this table.
		//
		// @return The row count.
		size_t Rows() const {
			return Ids.size();
		}

		// The column for one component, or null when this table has no such
		// component.
		//
		// @param id The component to find.
		// @return The column, or `nullptr`.
		Column *Find(ComponentId id);

		// The column for one component, read-only.
		//
		// @param id The component to find.
		// @return The column, or `nullptr`.
		const Column *Find(ComponentId id) const;

		// The column at a position in the set's sorted id list.
		//
		// For a caller that resolved its terms once and is iterating many
		// tables: the position is the same for every table holding that set.
		//
		// @param position The index into Set().Ids().
		// @return The column.
		Column &ColumnAt(size_t position) {
			return Columns[position];
		}

		// The entity ids, one per row, in row order.
		//
		// @return A view valid until the next structural change.
		const std::vector<Entity> &Entities() const {
			return Ids;
		}

		// The entity in one row.
		//
		// @param row The row, which must be less than Rows().
		// @return The entity.
		Entity EntityAt(size_t row) const {
			return Ids[row];
		}

		// Appends a row for `entity`, default-constructing every component.
		//
		// @param entity The entity taking the new row.
		// @return The new row's index.
		size_t Append(Entity entity);

		// Removes one row by moving the last row into it.
		//
		// @param row The row to remove.
		// @return The entity that moved into `row`, or NULL_ENTITY when the
		//         removed row was the last one and nothing moved.
		Entity RemoveSwapBack(size_t row);

		// Grows every column to hold at least `rows`.
		//
		// @param rows The row capacity to guarantee.
		void Reserve(size_t rows);

		// Moves one entity's components out of `source` into a new row here.
		//
		// Components this table does not hold are dropped; components it holds
		// that the source did not are default-constructed. That covers both
		// directions of a structural change with one operation.
		//
		// The source row is **not** removed — the caller does that, because it
		// also has to fix up whichever entity the removal moves.
		//
		// @param source    The table to move from.
		// @param sourceRow The row to move.
		// @param entity    The entity taking the new row.
		// @return The new row's index.
		size_t AdoptRow(Archetype &source, size_t sourceRow, Entity entity);

		// Appends this table's rows to a writer: the entity ids, then every
		// column in set order.
		//
		// The component set is *not* written here. Only the store knows how to
		// record it — as names, so a restore in another process resolves them
		// to whatever ids it assigned.
		//
		// @param writer The writer to append to.
		// @return `false` when a component has no serialisation, in which case
		//         nothing usable was written.
		bool Write(core::ByteWriter &writer) const;

		// Replaces this table's contents with `rows` rows read from `reader`.
		//
		// @param reader The reader to consume.
		// @param rows   The row count the caller read from the header.
		// @return `false` on a short or corrupt buffer, leaving the table empty.
		bool Read(core::ByteReader &reader, size_t rows);

	  private:
		// The interned set, which lives for the process.
		const ComponentSet *Members;

		// One per component in Members, in the set's sorted order — so a term
		// resolved to a position stays valid for every table sharing the set.
		std::vector<Column> Columns;

		std::vector<Entity> Ids;
	};
}
