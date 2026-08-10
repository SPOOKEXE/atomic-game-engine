#pragma once

// Keeping a world ticking when nobody is in it.
//
// **The half of occupancy a host cannot work out for itself.**
// `world::DecideLifecycle` suspends a world nobody is using, and what a host can
// see is players and — in an editor — viewports. A world whose NPCs are walking
// a route, whose shop is restocking, or which is counting down between rounds is
// indistinguishable from an abandoned one from there. `scene::AwakeWorld` is the
// game saying otherwise, and this is the pair of calls that read and write it.
//
// **A claim belongs to an entity, so its lifetime is the entity's.** Destroy the
// NPC and the world it was keeping awake becomes eligible again with nothing to
// remember. That is the failure a world-level flag has: somebody sets it, the
// code that would have cleared it never runs, and the world is immortal for a
// reason nobody can find.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Says this entity needs its world to keep ticking.
	//
	// Replaces the reason when one is already attached, so a caller may restate
	// it every tick without accumulating anything.
	//
	// @param store The world.
	// @param instance What needs the world awake.
	// @param reason Why, for a log or a panel. See `AwakeWorld::Reason` for why
	//        this is not optional.
	// @return `false` when `instance` is not alive, in which case nothing was
	//         written.
	bool KeepWorldAwake(ecs::Store &store, ecs::Entity instance, core::Name reason);

	// Withdraws this entity's claim.
	//
	// **Not an error when there was none.** A script tidying up should not have
	// to remember whether it made the claim, which is the same argument
	// `SetNetworkOwner` makes for handing a body back.
	//
	// @param store The world.
	// @param instance The entity withdrawing.
	void LetWorldSleep(ecs::Store &store, ecs::Entity instance);

	// Whether this entity is holding its world awake.
	//
	// @param store The world.
	// @param instance The entity being asked about.
	// @return `true` when it carries a claim.
	bool HoldsWorldAwake(const ecs::Store &store, ecs::Entity instance);

	// Whether anything at all is holding this world awake.
	//
	// What a host calls once per world per tick. Cheap on the shape every game
	// actually has: it stops at the first row, and a game that never attaches
	// one has none.
	//
	// **Takes a mutable store because `Store::Each` is not const**, which is a
	// property of the walk rather than of this question.
	//
	// @param store The world.
	// @param[out] reason Filled with the first claim's reason when one is found,
	//        so a host can say *what* is keeping a world up rather than only
	//        that something is. Untouched when the answer is `false`.
	// @return `true` when at least one entity carries a claim.
	bool WorldIsHeldAwake(ecs::Store &store, core::Name *reason = nullptr);
}
