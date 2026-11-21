#pragma once

// Which instances are in the world: `Workspace` and its descendants.
//
// Roblox simulates a part only while it is under Workspace. Everything else in
// the tree - `ServerStorage`, `ReplicatedStorage`, a template under
// `StarterGui`, an instance a script has not parented yet - is storage. The
// renderer already follows that rule through `Rendered` (see Visibility.hpp);
// physics did not, so an imported tower defence game with its maps kept in
// ServerStorage collided with, and rebuilt its static index over, about 200k
// stored parts it never showed.
//
// A world with no Workspace at all has nothing to call storage, so nothing in it
// is marked.
//
// `OutsideWorld` marks instances that are not in the world. It is a marker for
// the exception rather than for membership, so rows that are not instances at
// all - a test's bare Transform and Collider, a benchmark's bodies - stay in the
// world without anybody tagging them. Physics walks skip marked rows with
// `Without<OutsideWorld>()`.
//
// Kept by `SyncWorldMembership`, a sweep in the same spirit as `SyncRendered`:
// derived from the tree, never maintained by reparent hooks. After the first
// full walk it re-walks only the subtrees whose parent changed, and walks
// everything again when it cannot tell what changed.
//
// @tier L7 · shared

#include <engine/ecs/StoreFwd.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace engine::scene {

	// On an instance that is not Workspace or one of its descendants.
	//
	// Derived; written only by `SyncWorldMembership`. Never replicated or saved
	// as authored data: every host derives its own from its own tree.
	//
	// @since v0.26
	struct OutsideWorld {};

	// What `SyncWorldMembership` compares the tree against. Derived state: it
	// saves nothing and loads empty, so the first sync after a load walks.
	//
	// @since v0.26
	struct WorldMembershipMemo {
		// Whether a full walk has run since the memo was created or loaded.
		bool Fresh = false;
		// The Workspace the last walk used.
		uint64_t Workspace = 0;
		// Hierarchy write and membership epochs the last sync consumed.
		uint64_t HierarchyWrites = 0;
		uint64_t HierarchyMembership = 0;
		// Instance class writes and membership, which can change which root is Workspace.
		uint64_t ClassWrites = 0;
		uint64_t ClassMembership = 0;
		// Each instance's parent at the last sync, to tell a moved subtree from a
		// row whose sibling or child links changed around it.
		std::unordered_map<uint64_t, uint64_t> Parents;
		// Size of `Parents` after its last prune.
		size_t ParentsKept = 0;
	};

	// Brings `OutsideWorld` in line with the tree.
	//
	// @param store The world.
	// @return How many rows carry `OutsideWorld` afterwards.
	size_t SyncWorldMembership(ecs::Store &store);
}
