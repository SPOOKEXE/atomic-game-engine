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
		// column at that position is the one — the two arrays are built
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
			// inconsistency — there is no way for the store to work it out
			// afterwards.
			moved = Ids[last];
			Ids[row] = moved;
		}

		Ids.pop_back();
		return moved;
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
		// merge — linear in the wider of the two rather than n log n.
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

	bool Archetype::Read(core::ByteReader &reader, size_t rows) {
		Ids.clear();
		Ids.reserve(rows);

		for (size_t row = 0; row < rows; row++) {
			Ids.push_back(Entity{reader.ReadUInt64()});
		}
		if (reader.Failed()) {
			Ids.clear();
			return false;
		}

		for (Column &column : Columns) {
			if (!column.Read(reader, rows)) {
				Ids.clear();
				return false;
			}
		}

		return true;
	}
}
