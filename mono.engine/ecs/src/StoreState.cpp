#include "StoreState.hpp"

#include <engine/ecs/ChangeChannel.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Time.hpp>

#include <algorithm>
#include <new>
#include <span>

namespace engine::ecs {

	namespace {
		// Whether a component's writes are recorded in this world.
		bool Watching(const StoreState &state, ComponentId id) {
			return std::find(state.Watched.begin(), state.Watched.end(), id) != state.Watched.end();
		}

		// The set a table actually holds, which gains a DirtyBits column when
		// any member of it is observed.
		//
		// Done here rather than at every call site so that there is one answer
		// to "does this table track changes" and no way to create a table that
		// should have tracked and does not.
		const ComponentSet &Tracked(const StoreState &state, const ComponentSet &set) {
			if (state.Watched.empty() || set.IsEmpty()) {
				return set;
			}

			for (const ComponentId id : set.Ids()) {
				if (Watching(state, id)) {
					return set.With(Components::Of<DirtyBits>());
				}
			}
			return set;
		}

		// Records that one component of one row was written.
		//
		// The bit index is the component's position in the table's sorted set,
		// which is the same position its column sits at — so this is an index
		// the caller already has rather than a second lookup.
		void MarkWritten(StoreState &state, uint32_t table, uint32_t row, ComponentId id) {
			if (state.Watched.empty()) {
				return;
			}

			Archetype &archetype = state.Tables[table];
			Column *bits = archetype.Find(Components::Of<DirtyBits>());
			if (bits == nullptr) {
				return;
			}

			const std::span<const ComponentId> ids = archetype.Set().Ids();
			const auto at = std::lower_bound(ids.begin(), ids.end(), id);
			if (at == ids.end() || *at != id) {
				return;
			}

			static_cast<DirtyBits *>(bits->At(row))->Mark(static_cast<size_t>(at - ids.begin()));
			state.Changes++;
		}
	}

	bool WatchComponent(StoreState &state, ComponentId id) {
		if (!id.IsValid() || Watching(state, id)) {
			return false;
		}

		state.Watched.push_back(id);

		// The edges are now wrong rather than merely cold: a table holding this
		// component needs a `DirtyBits` column from here on, so a transition
		// into it has a different destination than the one already recorded.
		state.WatchEpoch++;
		state.Edges.Forget();
		return true;
	}

	uint32_t TableFor(StoreState &state, const ComponentSet &wanted) {
		const ComponentSet &set = Tracked(state, wanted);
		const auto found = state.TableBySet.find(set.Id());
		if (found != state.TableBySet.end()) {
			return found->second;
		}

		const auto index = static_cast<uint32_t>(state.Tables.size());
		state.Tables.emplace_back(set);
		state.TableBySet.emplace(set.Id(), index);
		return index;
	}

	void Relocate(StoreState &state, uint32_t index, EntityLocation from, uint32_t toTable) {
		Archetype &destination = state.Tables[toTable];
		const Entity entity = EntityId::Pack(index, state.Directory.Generation(index));

		uint32_t row = 0;
		if (from.Archetype == EntityLocation::NO_ARCHETYPE) {
			row = static_cast<uint32_t>(destination.Append(entity));
		} else {
			Archetype &source = state.Tables[from.Archetype];
			row = static_cast<uint32_t>(destination.AdoptRow(source, from.Row, entity));

			const Entity moved = source.RemoveSwapBack(from.Row);
			if (moved != NULL_ENTITY) {
				// The swap-back put somebody else where this row was, and their
				// directory entry still says otherwise.
				state.Directory.Relocate(EntityId::Of(moved).Index, EntityLocation{from.Archetype, from.Row});
			}
		}

		state.Directory.Relocate(index, EntityLocation{toTable, row});
	}

	void Vacate(StoreState &state, EntityLocation from) {
		if (from.Archetype == EntityLocation::NO_ARCHETYPE) {
			return;
		}

		Archetype &table = state.Tables[from.Archetype];
		const Entity moved = table.RemoveSwapBack(from.Row);
		if (moved != NULL_ENTITY) {
			state.Directory.Relocate(EntityId::Of(moved).Index, EntityLocation{from.Archetype, from.Row});
		}
	}

	Entity CreateEntity(StoreState &state, EntityRange range) {
		const uint32_t index = state.Directory.Allocate(range);
		if (index == SparseSet::NO_INDEX) {
			// The range is full. Handed back as the null handle so that every
			// caller's existing "did this work" check covers it, rather than as
			// an index from the other range that would silently collide.
			return NULL_ENTITY;
		}
		return EntityId::Pack(index, state.Directory.Generation(index));
	}

	void DestroyEntity(StoreState &state, Entity entity) {
		const EntityId id = EntityId::Of(entity);
		if (!state.Directory.Alive(id.Index, id.Generation)) {
			return;
		}

		if (state.DeferDepth > 0) {
			state.Commands.push_back(Command{Command::Kind::Destroy, entity, ComponentId{}, nullptr});
			return;
		}

		Vacate(state, *state.Directory.Locate(id.Index));

		const auto named = state.NamesByIndex.find(id.Index);
		if (named != state.NamesByIndex.end()) {
			state.EntitiesByName.erase(named->second);
			state.NamesByIndex.erase(named);
		}

		state.Directory.Free(id.Index);
	}

	bool IsEntityAlive(const StoreState &state, Entity entity) {
		if (entity.Id == 0) {
			return false;
		}
		const EntityId id = EntityId::Of(entity);
		return state.Directory.Alive(id.Index, id.Generation);
	}

	void SetComponent(StoreState &state, Entity entity, ComponentId id, const void *value) {
		const EntityId key = EntityId::Of(entity);
		if (!state.Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return;
		}

		if (state.DeferDepth > 0) {
			const TypeDescriptor &descriptor = Components::Describe(id);

			// The value is copied because the caller's may be a temporary that
			// is gone by the time the loop ends. Allocated at the component's
			// own alignment rather than the allocator's default, since a
			// component may be over-aligned.
			void *payload = nullptr;
			if (descriptor.Size > 0) {
				payload = ::operator new(descriptor.Size, std::align_val_t(descriptor.Alignment));
				descriptor.CopyConstruct(payload, value, 1);
			}

			state.Commands.push_back(Command{Command::Kind::Set, entity, id, payload});
			return;
		}

		const EntityLocation location = *state.Directory.Locate(key.Index);

		if (location.Archetype != EntityLocation::NO_ARCHETYPE) {
			if (Column *column = state.Tables[location.Archetype].Find(id); column != nullptr) {
				// Already present, so this is an overwrite and nothing moves.
				column->Assign(location.Row, value);
				MarkWritten(state, location.Archetype, location.Row, id);
				return;
			}
		}

		// An entity with no row yet has no table to hang an edge off, so the
		// cache covers table-to-table transitions only. That is the case worth
		// covering: the first component an entity gains happens once, and the
		// ones after it happen every time anything toggles.
		uint32_t destination = location.Archetype == EntityLocation::NO_ARCHETYPE
								   ? ArchetypeEdges::NO_TABLE
								   : state.Edges.Added(state.WatchEpoch, location.Archetype, id);

		if (destination == ArchetypeEdges::NO_TABLE) {
			const ComponentSet &current = location.Archetype == EntityLocation::NO_ARCHETYPE
											  ? ComponentSet::Empty()
											  : state.Tables[location.Archetype].Set();

			destination = TableFor(state, current.With(id));

			if (location.Archetype != EntityLocation::NO_ARCHETYPE) {
				state.Edges.RecordAddition(state.WatchEpoch, location.Archetype, id, destination);
			}
		}

		Relocate(state, key.Index, location, destination);

		const EntityLocation moved = *state.Directory.Locate(key.Index);
		state.Tables[moved.Archetype].Find(id)->Assign(moved.Row, value);

		// A component that has just appeared has changed as surely as one that
		// was overwritten, and a consumer rebuilding from a delta needs it.
		MarkWritten(state, moved.Archetype, moved.Row, id);
	}

	bool HasComponent(const StoreState &state, Entity entity, ComponentId id) {
		const EntityId key = EntityId::Of(entity);
		if (!state.Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return false;
		}

		const EntityLocation *location = state.Directory.Locate(key.Index);
		if (location->Archetype == EntityLocation::NO_ARCHETYPE) {
			return false;
		}
		return state.Tables[location->Archetype].Set().Contains(id);
	}

	const void *GetComponent(const StoreState &state, Entity entity, ComponentId id) {
		const EntityId key = EntityId::Of(entity);
		if (!state.Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return nullptr;
		}

		const EntityLocation *location = state.Directory.Locate(key.Index);
		if (location->Archetype == EntityLocation::NO_ARCHETYPE) {
			return nullptr;
		}

		const Column *column = state.Tables[location->Archetype].Find(id);
		if (column == nullptr) {
			return nullptr;
		}

		// A component with no data has no bytes to point at, but present and
		// absent still have to be distinguishable — so a tag reports the column
		// itself. Dereferencing it is meaningless for an empty type either way;
		// what matters is that null means absent.
		return column->Describe().Size == 0 ? static_cast<const void *>(column) : column->At(location->Row);
	}

	void *GetComponentMutable(StoreState &state, Entity entity, ComponentId id) {
		void *value = const_cast<void *>(GetComponent(state, entity, id));

		// Handing out a mutable pointer counts as a write. Whether the caller
		// used it is not knowable from here, and a change reported that did not
		// happen costs a consumer one wasted rebuild — where a change missed
		// costs it correctness.
		if (value != nullptr && !state.Watched.empty()) {
			const EntityId key = EntityId::Of(entity);
			const EntityLocation *location = state.Directory.Locate(key.Index);
			if (location != nullptr && location->Archetype != EntityLocation::NO_ARCHETYPE) {
				MarkWritten(state, location->Archetype, location->Row, id);
			}
		}

		return value;
	}

	void RemoveComponent(StoreState &state, Entity entity, ComponentId id) {
		const EntityId key = EntityId::Of(entity);
		if (!state.Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return;
		}

		if (state.DeferDepth > 0) {
			state.Commands.push_back(Command{Command::Kind::Remove, entity, id, nullptr});
			return;
		}

		const EntityLocation location = *state.Directory.Locate(key.Index);
		if (location.Archetype == EntityLocation::NO_ARCHETYPE) {
			return;
		}

		const uint32_t cached = state.Edges.Removed(state.WatchEpoch, location.Archetype, id);
		if (cached != ArchetypeEdges::NO_TABLE) {
			// Only a transition that produced a table is ever recorded, so a hit
			// here already means the component was present and something was
			// left over — both of the checks below have been answered once and
			// cannot have changed, because a table's set does not.
			Relocate(state, key.Index, location, cached);
			return;
		}

		const ComponentSet &current = state.Tables[location.Archetype].Set();
		if (!current.Contains(id)) {
			return;
		}

		const ComponentSet &reduced = current.Without(id);
		if (reduced.IsEmpty()) {
			// Back to no components at all, which is a directory slot and no row
			// rather than a row in a table of nothing. Deliberately not cached:
			// there is no destination table to name, and a sentinel meaning
			// "nowhere" would be a second thing every reader has to handle.
			Vacate(state, location);
			state.Directory.Relocate(key.Index, EntityLocation{});
			return;
		}

		const uint32_t destination = TableFor(state, reduced);
		state.Edges.RecordRemoval(state.WatchEpoch, location.Archetype, id, destination);
		Relocate(state, key.Index, location, destination);
	}

	void SetResourceValue(StoreState &state, ComponentId id, const void *value) {
		if (!id.IsValid()) {
			return;
		}

		Column &column = state.Resources.Reach(id);
		if (column.Empty()) {
			column.PushCopy(value);
		} else {
			column.Assign(0, value);
		}
	}

	const void *GetResourceValue(const StoreState &state, ComponentId id) {
		const Column *found = state.Resources.Find(id.Index);
		if (found == nullptr || found->Empty()) {
			return nullptr;
		}

		const Column &column = *found;
		return column.Describe().Size == 0 ? static_cast<const void *>(&column) : column.At(0);
	}

	void ClearWorld(StoreState &state) {
		state.Tables.clear();
		state.TableBySet.clear();

		// Before anything else creates a table: an edge names a table by index,
		// and index zero is about to belong to a different world.
		state.Edges.Forget();
		state.Directory.Clear();
		state.NamesByIndex.clear();
		state.EntitiesByName.clear();
		state.Resources.Clear();
		state.Plans.clear();
		state.Commands.clear();
		state.Watched.clear();
		state.DeferDepth = 0;

		const WorldTime clock{};
		SetResourceValue(state, Components::Of<WorldTime>(), &clock);
	}
}
