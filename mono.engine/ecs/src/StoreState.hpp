#pragma once

// The storage behind one Store, the query cache over it, and the primitives
// that move rows around inside it.
//
// Private, because it is the layout. `Store`'s public header promises what a
// world can do; this is how, and it is expected to change — see `ecs/AGENTS.md`
// on everything public here being a migration cost.
//
// The primitives at the bottom are free functions rather than members of
// anything, because `Store` is not the only caller: the instance façade in
// `Instances.hpp` and the snapshot codec in `Snapshot.hpp` are built from the
// same handful of operations, and a second copy of "move this row" is the one
// kind of duplication this layout cannot survive.
//
// @tier L3 · shared

#include "Archetype.hpp"

#include <engine/ecs/ComponentSet.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/SparseSet.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

	class Store;

	// How an entity handle packs an index and a generation.
	//
	// Kept here rather than on `Entity`, which says so itself: reading the
	// layout from outside the store is how code starts depending on it. The
	// generation is in the high bits and starts at one, so no live entity is
	// ever all-zero — which is the null handle.
	struct EntityId {
		// The index into the directory.
		uint32_t Index = 0;

		// The generation the handle was issued at.
		uint32_t Generation = 0;

		// Unpacks a handle.
		//
		// @param entity The handle to split.
		// @return Its index and generation.
		static EntityId Of(Entity entity) {
			return EntityId{
				static_cast<uint32_t>(entity.Id & 0xFFFF'FFFFull), static_cast<uint32_t>(entity.Id >> 32)
			};
		}

		// Packs an index and generation into a handle.
		//
		// @param index      The directory index.
		// @param generation The generation.
		// @return The handle.
		static Entity Pack(uint32_t index, uint32_t generation) {
			return Entity{(static_cast<uint64_t>(generation) << 32) | index};
		}
	};

	// One deferred structural change.
	//
	// `Each` runs with deferral on, so a system may create, destroy or
	// re-component entities inside a loop without moving the rows the loop is
	// walking. The change lands when the loop ends.
	struct Command {
		enum class Kind : uint8_t {
			Destroy,
			Set,
			Remove,
		};

		Kind What = Kind::Destroy;
		Entity Target;
		ComponentId Component;

		// A copy of the value for Set, owned by the command and destroyed with
		// it. Allocated at the component's alignment rather than the
		// allocator's default, because a component may be over-aligned and a
		// vector of bytes does not promise more than max_align_t.
		void *Payload = nullptr;
	};

	// A query's resolved plan: which tables match, and where each term sits in
	// them.
	//
	// Built once per term list and kept, so a system may iterate every tick
	// without rebuilding it. `D00003` is the deferred item this closes.
	struct QueryPlan {
		// The terms, sorted, which is what the cache is keyed by.
		std::vector<ComponentId> Terms;

		// One entry per matching table.
		struct Match {
			uint32_t Table = 0;

			// Where each term's column sits in that table, in the *caller's*
			// term order rather than sorted order — so the iteration code can
			// index it directly by parameter position.
			std::vector<size_t> Positions;
		};

		std::vector<Match> Matches;

		// The archetype count this plan was built against. A table created
		// afterwards may also match, so the plan is topped up rather than
		// rebuilt when this falls behind.
		size_t SeenTables = 0;
	};

	// Everything one world owns.
	struct StoreState {
		// The tables. A deque so that a reference to one survives another being
		// created — a query plan holds indices, but the flush path holds
		// references across an append.
		std::deque<Archetype> Tables;

		// Interned set id to table index, which is what makes finding the table
		// for a set a hash of one number.
		std::unordered_map<uint32_t, uint32_t> TableBySet;

		// Entity index to generation and location.
		SparseSet Directory;

		// Named entities, both ways. Names are optional and most entities have
		// none, so this is a map rather than a column.
		std::unordered_map<uint32_t, std::string> NamesByIndex;
		std::unordered_map<std::string, Entity> EntitiesByName;

		// World-scoped values, one per type.
		//
		// A map of single-row columns rather than a row on a hidden entity.
		// That is what makes a resource structurally invisible to a query —
		// with a hidden entity it was invisible only because somebody
		// remembered to disable it.
		std::unordered_map<uint32_t, Column> Resources;

		// Query plans, keyed by the sorted term list.
		std::unordered_map<std::string, QueryPlan> Plans;

		// Deferred structural changes, and whether we are collecting them.
		std::vector<Command> Commands;
		int DeferDepth = 0;

		// Component types whose writes are recorded, and the coarse counter a
		// batch write still moves.
		std::vector<ComponentId> Watched;
		uint64_t Changes = 0;

		// One registered change signal.
		struct Listener {
			// The component whose writes it wants.
			ComponentId Subject;

			// What `Connection` names, so a disconnect is a search for a number
			// rather than for a callable — two `std::function`s are not
			// comparable and never will be.
			uint64_t Id = 0;

			// Called with the store, the entity, and a pointer to the current
			// value. Re-fetched per entity rather than captured, because a
			// listener before it may have moved the row.
			std::function<void(Store &, Entity, const void *)> Body;
		};

		std::vector<Listener> Listeners;
		uint64_t NextConnection = 1;

		// Whether a flush is running. A listener that writes must not re-enter
		// the flush: the write it just made belongs to the *next* boundary, and
		// firing it now is the mid-batch behaviour signals exist to avoid.
		bool Flushing = false;

		// Reused across flushes so a world with signals stops allocating.
		//
		// Collected before anything fires, because a listener may add or remove
		// a component and move the rows out from under an iteration.
		std::vector<Entity> Firing;
	};

	// None of what follows checks thread affinity. That belongs at the call the
	// caller actually made — `Store::DestroyInstance` is one abort site, not one
	// per row it touches — and a check inside a primitive would be able to fire
	// from the middle of a half-applied structural change.

	// The table for a set, creating it when this world has not needed one.
	//
	// @param state  The world to look in.
	// @param wanted The set the caller is after, before change tracking has had
	//               its say.
	// @return The table index, valid until the world is cleared.
	uint32_t TableFor(StoreState &state, const ComponentSet &wanted);

	// Moves an entity's row into another table and fixes up whoever the removal
	// displaced.
	//
	// @param state   The world holding both tables.
	// @param index   The directory index of the entity to move.
	// @param from    Where its row is now, or a default location when it has no
	//                row yet.
	// @param toTable The table to move it into.
	void Relocate(StoreState &state, uint32_t index, EntityLocation from, uint32_t toTable);

	// Drops an entity's row, leaving the directory alone.
	//
	// @param state The world holding the row.
	// @param from  The row to drop. A location with no archetype does nothing.
	void Vacate(StoreState &state, EntityLocation from);

	// Takes a directory slot.
	//
	// @param state The world to allocate in.
	// @return A live handle carrying no components and occupying no row.
	Entity CreateEntity(StoreState &state);

	// Frees an entity's row, its name and its directory slot.
	//
	// Deferred when the world is collecting commands.
	//
	// @param state  The world to remove it from.
	// @param entity The generation to destroy. A stale handle does nothing.
	void DestroyEntity(StoreState &state, Entity entity);

	// Reports whether a handle names a live entity at the generation it was
	// issued with.
	//
	// @param state  The world to ask.
	// @param entity The handle to inspect.
	// @return `false` for NULL_ENTITY, destroyed entities and stale generations.
	bool IsEntityAlive(const StoreState &state, Entity entity);

	// Adds or replaces one component value, moving the row when the component
	// is new to it.
	//
	// Deferred when the world is collecting commands, in which case the value
	// is copied and the copy owned by the command.
	//
	// @param state  The world to write into.
	// @param entity The entity that owns the component.
	// @param id     The component to write.
	// @param value  The value to copy in.
	void SetComponent(StoreState &state, Entity entity, ComponentId id, const void *value);

	// Reports whether an entity carries a component.
	//
	// @param state  The world to ask.
	// @param entity The entity to inspect.
	// @param id     The component to look for.
	// @return `true` when the component is present.
	bool HasComponent(const StoreState &state, Entity entity, ComponentId id);

	// Reads one component value.
	//
	// @param state  The world to read from.
	// @param entity The entity that owns the component.
	// @param id     The component to read.
	// @return A pointer into the row, or `nullptr` when the component is
	//         absent. A component with no data reports its column instead, so
	//         that present and absent stay distinguishable.
	const void *GetComponent(const StoreState &state, Entity entity, ComponentId id);

	// Reads one component value for writing, and records the write.
	//
	// @param state  The world to read from.
	// @param entity The entity that owns the component.
	// @param id     The component to read.
	// @return A pointer into the row, or `nullptr` when the component is absent.
	void *GetComponentMutable(StoreState &state, Entity entity, ComponentId id);

	// Removes one component, moving the row to the reduced set.
	//
	// Deferred when the world is collecting commands.
	//
	// @param state  The world to write into.
	// @param entity The entity to take it from.
	// @param id     The component to remove.
	void RemoveComponent(StoreState &state, Entity entity, ComponentId id);

	// Adds or replaces one world-scoped value.
	//
	// @param state The world to write into.
	// @param id    The resource type.
	// @param value The value to copy in.
	void SetResourceValue(StoreState &state, ComponentId id, const void *value);

	// Reads one world-scoped value.
	//
	// @param state The world to read from.
	// @param id    The resource type.
	// @return A pointer to the value, or `nullptr` when unset.
	const void *GetResourceValue(const StoreState &state, ComponentId id);

	// Empties a world and gives it a fresh clock.
	//
	// Signals survive. A listener is a registration made by whoever owns the
	// world rather than part of its contents, and dropping them here would
	// disconnect every consumer at the moment a snapshot was loaded.
	//
	// @param state The world to empty.
	void ClearWorld(StoreState &state);
}
