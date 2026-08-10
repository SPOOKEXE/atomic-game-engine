#pragma once

// Who simulates a body.
//
// The server, unless a `NetworkOwner` says otherwise — which is the default a
// world gets by attaching nothing at all, and the reason ownership costs an
// entirely server-owned game one archetype nobody visits.
//
// **Nothing in the engine reads the answer yet.** Physics still integrates every
// body wherever it runs and `replication::Authority` still sends every
// replicated component to every interested client. This module is ownership made
// expressible and observable ahead of being load-bearing: making it load-bearing
// means state travelling client→server, which is a wire change with a trust
// decision inside it rather than a component and two accessors.
//
// What is *already* real is the reclaim below, because that half has a correct
// answer today and would be a bug the day the rest lands.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::scene {

	// Hands a body to a player, or back to the server.
	//
	// **Passing a null entity removes the component rather than storing one.**
	// Absent and "owned by nobody" would be two spellings of the same state, and
	// the first thing to go wrong with two spellings is a query that checks one.
	//
	// @param store    The world.
	// @param instance What to hand over.
	// @param player   The `Player` instance that will simulate it, or
	//                 `ecs::NULL_ENTITY` to give it back to the server.
	// @return `false` when `player` is neither null nor a live `Player`, in
	//         which case nothing was written. A caller with a script behind it
	//         should raise; a caller without one should not pretend it worked.
	bool SetNetworkOwner(ecs::Store &store, ecs::Entity instance, ecs::Entity player);

	// Who simulates `instance`.
	//
	// @param store    The world.
	// @param instance The thing being asked about.
	// @return The owning `Player`, or `ecs::NULL_ENTITY` for the server — which
	//         is also the answer for an instance that has never been assigned.
	ecs::Entity NetworkOwnerOf(const ecs::Store &store, ecs::Entity instance);

	// Gives back everything owned by a player who is no longer here.
	//
	// **A player leaving must not leave bodies owned by a hole.** An `Entity`
	// carries a generation, so a destroyed player's handle does not silently
	// become whoever is created next — but it does stop being alive, and an
	// owner that is not alive is an entity nothing will ever simulate. Reverting
	// to the server is the only answer that leaves the world running.
	//
	// Cheap on the shape every game actually has: it visits `NetworkOwner` rows,
	// and a game that hands nothing to anybody has none.
	//
	// @param store The world.
	void ReclaimAbandonedOwnership(ecs::Store &store);

	// Adds `ReclaimAbandonedOwnership` to a world as `scene.ownership`.
	//
	// `Phase::PreSimulation`, so a body whose owner left this tick is the
	// server's again *before* anything simulates it rather than one tick after.
	// It is also structural — it removes a component — and PreSimulation is
	// where this engine puts structural work.
	//
	// **On the authority, not on a replica.** Who owns what is the host's answer
	// and it crosses the wire like any other `scene.` component, so a client
	// running this would delete a row the next delta puts straight back — a
	// disagreement every tick between two machines that are not disagreeing.
	//
	// @param scheduler The scheduler to add to.
	void RegisterOwnershipSystem(ecs::Scheduler &scheduler);
}
