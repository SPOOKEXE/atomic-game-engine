#pragma once

// Where a row lands when one component is added to it or taken away.
//
// Adding a component an entity lacks moves its row to a different table, and
// finding that table used to be rebuilt from first principles every time:
// `set.With(id)` allocates a vector, sorts it, hashes it and looks it up under a
// process-wide mutex, and only then does `TableFor` hash the resulting set id to
// reach the table. Both halves produce the same answer every time for the same
// starting table and the same component, because a table's component set never
// changes — so the second time is a cache miss that had all the information to
// be a hit.
//
// **This was built against a measurement, not a hunch.** `ecs/docs/TODO.md`
// gated it on one: *the number to have first is what archetype transitions cost
// as a fraction of a tick*. `benchmarks/Structure.cpp` is that number, and it
// says a transition is roughly fifty nanoseconds against nine for overwriting a
// component already present — so changing an entity's shape costs about five
// times what writing to it does, and rather more than iterating it.
//
// **A linear scan of a small vector, not a hash map.** An archetype has a
// handful of edges in practice: the components a gameplay tick actually toggles
// on the entities in one table. Scanning eight contiguous pairs beats hashing a
// composite key, and a hash lookup that cost as much as the interning it
// replaced would be a cache that bought nothing.
//
// **The one thing that invalidates an edge is `Observe`.** A table's set is
// fixed, so `(table, component)` maps to one destination forever — *unless*
// observing a component starts requiring a `DirtyBits` column, which changes
// what `Tracked` appends and therefore which table a transition should reach.
// That is why every lookup carries the watch epoch and a stale one wipes the
// cache rather than answering from it. An edge that outlived its epoch is not a
// slow answer, it is a wrong one: the row would land in a table with no bits and
// the change would go unreported.
//
// @tier L3 · shared

#include <engine/ecs/Components.hpp>

#include <cstdint>
#include <vector>

namespace engine::ecs {

	// Cached add-one and remove-one transitions, per table.
	//
	// @since v0.3
	class ArchetypeEdges {
	  public:
		// The answer when nothing is cached, which is not a table index.
		//
		// Distinct from any real index because a caller has to be able to tell
		// "no edge recorded" from "the edge leads to table zero", and table
		// zero is the first one any world creates.
		static constexpr uint32_t NO_TABLE = ~0u;

		// The table a row reaches by gaining one component.
		//
		// @param epoch     The store's current watch epoch.
		// @param from      The table the row is in.
		// @param component The component being added.
		// @return The destination table, or `NO_TABLE` when it is not cached.
		uint32_t Added(uint64_t epoch, uint32_t from, ComponentId component);

		// The table a row reaches by losing one component.
		//
		// @param epoch     The store's current watch epoch.
		// @param from      The table the row is in.
		// @param component The component being removed.
		// @return The destination table, or `NO_TABLE` when it is not cached.
		uint32_t Removed(uint64_t epoch, uint32_t from, ComponentId component);

		// Records where adding one component leads.
		//
		// @param epoch       The store's current watch epoch.
		// @param from        The table the row was in.
		// @param component   The component that was added.
		// @param destination The table it reached.
		void RecordAddition(uint64_t epoch, uint32_t from, ComponentId component, uint32_t destination);

		// Records where removing one component leads.
		//
		// @param epoch       The store's current watch epoch.
		// @param from        The table the row was in.
		// @param component   The component that was removed.
		// @param destination The table it reached.
		void RecordRemoval(uint64_t epoch, uint32_t from, ComponentId component, uint32_t destination);

		// Drops every edge.
		//
		// For a world being emptied: the table indices an edge names are about
		// to mean something else, and an edge that survived that would send a
		// row to a table belonging to the previous world.
		void Forget();

		// How many edges are held, across every table.
		//
		// For the suite, which has to be able to tell a hit from a miss without
		// timing anything.
		//
		// @return The number of recorded edges.
		size_t Count() const;

	  private:
		struct Edge {
			// The component's dense index, not the `ComponentId` — the compare
			// is the whole inner loop and comparing one integer is the point.
			uint32_t Component = 0;

			// Where the row lands.
			uint32_t Destination = 0;
		};

		struct Node {
			std::vector<Edge> Additions;
			std::vector<Edge> Removals;
		};

		// Discards everything when the epoch has moved on, and adopts the new
		// one. Called by every public entry point, so there is no path that
		// reads or writes an edge without first agreeing on the epoch.
		void Reconcile(uint64_t epoch);

		// Grows to cover `table`, since a table is created before its edges
		// are.
		Node &Reach(uint32_t table);

		static uint32_t Lookup(const std::vector<Edge> &edges, uint32_t component);
		static void Record(std::vector<Edge> &edges, uint32_t component, uint32_t destination);

		std::vector<Node> ByTable;
		uint64_t Epoch = 0;
	};
}
