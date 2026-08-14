#include "Archetype.hpp"

#include <algorithm>
#include <cstdint>

namespace engine::ecs {

	Archetype::Archetype(const ComponentSet &set) : Members(&set) {
		Columns.reserve(set.Size());
		for (const ComponentId id : set.Ids()) {
			Columns.emplace_back(id);
		}
	}

	Column *Archetype::Find(ComponentId id) {
		// A binary search over the set's sorted ids gives the position, and the
		// column at that position is the one - the two arrays are built
		// together and stay parallel for the table's whole life.
		const std::span<const ComponentId> ids = Members->Ids();
		const auto found = std::lower_bound(ids.begin(), ids.end(), id);
		if (found == ids.end() || *found != id) {
			return nullptr;
		}
		return &Columns[static_cast<size_t>(found - ids.begin())];
	}

	const Column *Archetype::Find(ComponentId id) const {
		return const_cast<Archetype *>(this)->Find(id);
	}

	size_t Archetype::ResidentBytes() const {
		size_t bytes = Ids.capacity() * sizeof(Entity);
		for (const Column &column : Columns) {
			bytes += column.ResidentBytes();
		}
		return bytes;
	}

	size_t Archetype::Append(Entity entity) {
		const size_t row = Ids.size();
		Ids.push_back(entity);

		for (Column &column : Columns) {
			column.PushDefault();
		}

		return row;
	}

	Entity Archetype::RemoveSwapBack(size_t row) {
		const size_t last = Ids.size() - 1;

		for (Column &column : Columns) {
			column.RemoveSwapBack(row);
		}

		Entity moved = NULL_ENTITY;
		if (row != last) {
			// The entity that used to be in the last row now lives in `row`,
			// and its directory entry still says otherwise. Returning it is
			// what makes that the caller's problem rather than a silent
			// inconsistency - there is no way for the store to work it out
			// afterwards.
			moved = Ids[last];
			Ids[row] = moved;
		}

		Ids.pop_back();
		TrimIds();
		return moved;
	}

	void Archetype::TrimIds() {
		// The id array is the one part of a row that is not a `Column`, and it
		// had the same leak: `pop_back` never returns capacity, so a table that
		// peaked at ten thousand rows kept eighty kilobytes of handles for a
		// hundred entities. At eight bytes against a thirty-two byte row that is
		// a quarter of what chunking gives back.
		//
		// Rebuilt rather than chunked. `VisitChangedRuns` hands a callback
		// `entities + start` beside a value pointer, and one contiguous array is
		// what keeps that a single addition - worth more than the bytes a second
		// chunk directory would save.
		//
		// A quarter full before it shrinks, and it shrinks to half: a population
		// has to double before it can shrink again, so a world oscillating
		// across the threshold does not rebuild on every oscillation. The copies
		// are geometric, so settling from a peak costs O(peak) once.
		if (Ids.capacity() < 4 * Ids.size() + 4) {
			return;
		}

		std::vector<Entity> tightened;
		tightened.reserve(2 * Ids.size() + 1);
		tightened.assign(Ids.begin(), Ids.end());
		Ids.swap(tightened);
	}

	void Archetype::Reserve(size_t rows) {
		Ids.reserve(rows);
		for (Column &column : Columns) {
			column.Reserve(rows);
		}
	}

	size_t Archetype::AdoptRow(Archetype &source, size_t sourceRow, Entity entity) {
		const size_t row = Ids.size();
		Ids.push_back(entity);

		// Walk both sorted id lists together rather than searching one for each
		// member of the other. Both are sorted by component id, so this is a
		// merge - linear in the wider of the two rather than n log n.
		const std::span<const ComponentId> theirs = source.Members->Ids();
		const std::span<const ComponentId> ours = Members->Ids();

		size_t here = 0;
		size_t there = 0;

		while (here < ours.size()) {
			while (there < theirs.size() && theirs[there] < ours[here]) {
				// A component the source had and this table does not. Dropped,
				// which is what removing a component means.
				there++;
			}

			if (there < theirs.size() && theirs[there] == ours[here]) {
				Columns[here].PushMovedFrom(source.Columns[there], sourceRow);
				there++;
			} else {
				// A component this table has and the source did not, which is
				// what adding one means. The caller assigns the value straight
				// after; defaulting first is what keeps the row constructed at
				// every point in between.
				Columns[here].PushDefault();
			}

			here++;
		}

		return row;
	}
}

namespace engine::ecs {

	bool Archetype::Write(core::ByteWriter &writer) const {
		for (const Column &column : Columns) {
			if (!column.Describe().Serialisable && column.Describe().Size > 0) {
				return false;
			}
		}

		// The entity ids first, because a reader needs them to put the
		// directory back before it can make sense of anything pointing at them.
		for (const Entity entity : Ids) {
			writer.WriteUInt64(entity.Id);
		}

		for (const Column &column : Columns) {
			if (!column.Write(writer)) {
				return false;
			}
		}

		return true;
	}

	bool Archetype::Read(core::ByteReader &reader, size_t rows, std::span<const ComponentId> order) {
		Ids.clear();
		Ids.reserve(rows);

		for (size_t row = 0; row < rows; row++) {
			Ids.push_back(Entity{reader.ReadUInt64()});
		}
		if (reader.Failed()) {
			Ids.clear();
			return false;
		}

		// This table's own order, which is only right when the same process
		// wrote the bytes. See the header for why that is a real distinction.
		if (order.empty()) {
			for (Column &column : Columns) {
				if (!column.Read(reader, rows)) {
					Ids.clear();
					return false;
				}
			}
			return true;
		}

		// **The writer's order, column by column, and every one of them has to
		// be found.** An id in `order` this table does not hold would mean the
		// caller interned a different set from the one it is reading, and there
		// is no way to skip a column whose length is only knowable by reading
		// it - so the whole table is refused rather than read halfway.
		for (const ComponentId id : order) {
			Column *column = Find(id);
			if (column == nullptr) {
				Ids.clear();
				return false;
			}
			if (!column->Read(reader, rows)) {
				Ids.clear();
				return false;
			}
		}

		return true;
	}
}
