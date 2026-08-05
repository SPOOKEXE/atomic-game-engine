#pragma once

// The Roblox-shaped façade, over the same rows everything else iterates.
//
// An instance is not an object. It is an entity in the archetype its class
// names, carrying `InstanceClass`, `Hierarchy` and `InstanceName` alongside
// whatever else that class holds — so a system that wants every part in the
// world writes a query rather than walking a tree.
//
// The tree here is **organisational, not spatial**. Parenting moves nothing and
// propagates nothing, because a part's transform is world-space; that is
// Roblox's model, and it is what lets the whole hierarchy be five entity
// handles per node with no traversal pass behind it. The sibling list is
// intrusive and doubly linked for the same reason a child vector was rejected:
// a vector per node is an allocation for every instance in a world, and most
// instances have no children at all.
//
// Private, because none of this is a second way to reach the storage. `Store`
// is the surface; these are the operations it forwards to, split out because
// the tree and the prototype copy are a self-contained job and the store is
// otherwise a file nobody can hold in their head.
//
// @tier L3 · shared

#include "StoreState.hpp"

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Instance.hpp>

#include <functional>
#include <string_view>

namespace engine::ecs {

	// Creates an instance of a class, starting from its prototype row.
	//
	// @param state The world to create it in.
	// @param id    The class to instantiate.
	// @param name  The instance's name, which need not be unique. An empty name
	//              leaves the instance unnamed.
	// @return The new instance, or NULL_ENTITY for an invalid class.
	Entity CreateInstance(StoreState &state, ClassId id, std::string_view name);

	// The class an entity was created as.
	//
	// @param state    The world to ask.
	// @param instance The entity to ask about.
	// @return The class, or an invalid id when it is not an instance.
	ClassId ClassOf(const StoreState &state, Entity instance);

	// Reports whether an instance is of a class or one derived from it.
	//
	// @param state    The world to ask.
	// @param instance The entity to test.
	// @param id       The class to test against.
	// @return `true` when the instance's class is `id` or descends from it.
	bool IsA(const StoreState &state, Entity instance, ClassId id);

	// The name an instance carries.
	//
	// @param state    The world to ask.
	// @param instance The instance to ask about.
	// @return The name, or an invalid Name when unnamed.
	core::Name InstanceNameOf(const StoreState &state, Entity instance);

	// The parent of an instance.
	//
	// @param state    The world to ask.
	// @param instance The instance to ask about.
	// @return The parent, or NULL_ENTITY for a root or a non-instance.
	Entity ParentOf(const StoreState &state, Entity instance);

	// Visits every child in insertion order.
	//
	// @param state    The world to walk.
	// @param instance The parent whose children to visit.
	// @param body     Called as `body(Entity)` for each child. It may reparent
	//                 or destroy the child it was handed.
	// Whether an instance has any children. O(1); see `Store::HasChildren`.
	bool HasChildren(const StoreState &state, Entity instance);

	void EachChild(const StoreState &state, Entity instance, const std::function<void(Entity)> &body);

	// The first child with a name, searching in insertion order.
	//
	// @param state    The world to search.
	// @param instance The parent to search under.
	// @param name     The name to find.
	// @return The child, or NULL_ENTITY when none matches.
	Entity FindFirstChild(const StoreState &state, Entity instance, std::string_view name);

	// Reports whether one instance is inside another's subtree.
	//
	// @param state    The world to walk.
	// @param instance The instance to test.
	// @param ancestor The subtree root to test against.
	// @return `true` when instance is ancestor or sits beneath it.
	bool IsDescendantOf(const StoreState &state, Entity instance, Entity ancestor);

	// Moves an instance under a new parent, or to no parent.
	//
	// @param state    The world holding both.
	// @param instance The instance to move.
	// @param parent   The new parent, or NULL_ENTITY to detach.
	// @return `false` when either end is not an instance, or when the move
	//         would make `instance` its own ancestor.
	bool SetParent(StoreState &state, Entity instance, Entity parent);

	// Destroys an instance and everything under it.
	//
	// @param state    The world to remove it from.
	// @param instance The root of the subtree to destroy.
	void DestroyInstance(StoreState &state, Entity instance);

	// Copies one instance, its components and its whole subtree.
	//
	// @param state  The world holding the source, which also receives the copy.
	// @param source The instance to copy.
	// @return The copy, parented nowhere, or NULL_ENTITY when the source is not
	//         an instance.
	Entity CloneInstance(StoreState &state, Entity source);
}
