#pragma once

// The storage behind one Store, and the query cache over it.
//
// Private, because it is the layout. `Store`'s public header promises what a
// world can do; this is how, and it is expected to change — see `ecs/AGENTS.md`
// on everything public here being a migration cost.
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
}
