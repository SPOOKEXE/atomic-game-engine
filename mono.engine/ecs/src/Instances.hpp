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
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {

	// Gives `InstanceClass`, `Hierarchy` and `InstanceName` their real names.
	//
	// **The three components every instance carries are `ecs::` types, so `ecs`
	// has to be what names them.** They had no explicit registration here at
	// all: `Hierarchy` was named by `scene`, one module up, and the other two
	// were left to `Components::Of<T>()`, which registers under whatever the
	// compiler spells the type as.
	//
	// Two things followed, and both were silent.
	//
	// **A name nothing guarantees ends up in a file.** A snapshot records
	// components by name, and `TypeNameOf` is `__PRETTY_FUNCTION__` — so the
	// name in the file was a property of the compiler that wrote it. That is
	// the exact reasoning `scene` gave for naming `Hierarchy` explicitly; it
	// applies just as well to the other two.
	//
	// **And whoever registered first decided the name.** `Components::Adopt`
	// refuses an explicit name for a type that already has an automatic one,
	// because a type cannot have two — so a program where anything touched a
	// `Hierarchy` before `scene` registered it *aborted*. Nothing declared the
	// order. In a shipped program `RegisterSceneComponents` happened to run
	// first; in a test binary the shuffle decided, and `test_replication` died
	// roughly one run in twenty-five with `component 'ecs.Hierarchy' is already
	// registered as 'Hierarchy'`.
	//
	// Called from every door into the instance model — a class registration and
	// a store's construction — because being first is the entire job. Repeating
	// it is a lock and three compares.
	void RegisterInstanceComponents();

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

	// Visits everything under an instance, nearest first.
	//
	// **Depth first, in the order a recursive walk written by hand would
	// produce** — a child, then everything under that child, then the next
	// child. That is Roblox's `GetDescendants` order, and scripts index into
	// the result, so it is a contract rather than an implementation detail.
	//
	// The instance itself is not visited. Iterative rather than recursive: a
	// scene's depth is the author's to choose, and recursion would put it on
	// the C stack.
	//
	// @param state    The world to walk.
	// @param instance The root of the walk, not itself visited.
	// @param body     Called as `body(Entity)` for each descendant.
	void EachDescendant(const StoreState &state, Entity instance, const std::function<void(Entity)> &body);

	// Renames an instance.
	//
	// @param state    The world holding it.
	// @param instance The instance to rename.
	// @param name     The new name. An empty one leaves it unnamed.
	// @return `false` when the entity is not an instance.
	bool SetInstanceName(StoreState &state, Entity instance, std::string_view name);

	// The first child with a name, optionally searching the whole subtree.
	//
	// @param state     The world to search.
	// @param instance  The parent to search under.
	// @param name      The name to find. An empty name matches nothing.
	// @param recursive Whether to search descendants as well as children.
	// @return The instance, or NULL_ENTITY when none matches.
	Entity FindFirstChild(const StoreState &state, Entity instance, std::string_view name, bool recursive);

	// The first child created as exactly a class.
	//
	// **Exactly, which is what separates it from `FindFirstChildWhichIsA`.** A
	// `Part` is a `BasePart`, so asking for a `BasePart` this way finds
	// nothing; that is Roblox's split and the reason both exist.
	//
	// @param state    The world to search.
	// @param instance The parent to search under.
	// @param id       The class to match.
	// @return The child, or NULL_ENTITY when none matches.
	Entity FindFirstChildOfClass(const StoreState &state, Entity instance, ClassId id);

	// The first child of a class or one derived from it.
	//
	// @param state     The world to search.
	// @param instance  The parent to search under.
	// @param id        The class to match against.
	// @param recursive Whether to search descendants as well as children.
	// @return The instance, or NULL_ENTITY when none matches.
	Entity FindFirstChildWhichIsA(const StoreState &state, Entity instance, ClassId id, bool recursive);

	// The nearest ancestor with a name.
	//
	// @param state    The world to walk.
	// @param instance The instance to search above. Not itself considered.
	// @param name     The name to find. An empty name matches nothing.
	// @return The ancestor, or NULL_ENTITY when none matches.
	Entity FindFirstAncestor(const StoreState &state, Entity instance, std::string_view name);

	// The nearest ancestor created as exactly a class.
	//
	// @param state    The world to walk.
	// @param instance The instance to search above. Not itself considered.
	// @param id       The class to match.
	// @return The ancestor, or NULL_ENTITY when none matches.
	Entity FindFirstAncestorOfClass(const StoreState &state, Entity instance, ClassId id);

	// The nearest ancestor of a class or one derived from it.
	//
	// @param state    The world to walk.
	// @param instance The instance to search above. Not itself considered.
	// @param id       The class to match against.
	// @return The ancestor, or NULL_ENTITY when none matches.
	Entity FindFirstAncestorWhichIsA(const StoreState &state, Entity instance, ClassId id);

	// The dotted path from the root of the tree down to an instance.
	//
	// @param state    The world to walk.
	// @param instance The instance to describe.
	// @return The path, or an empty string when it is not an instance.
	std::string GetFullName(const StoreState &state, Entity instance);

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

	// Takes an instance out of the tree without destroying anything.
	//
	// **What `Store::Destroy` owes the tree.** Freeing a row does not touch the
	// links that point *at* it, so a raw destroy used to leave a live parent
	// naming a freed child — and `EachChild` stops at the first dead link
	// rather than stepping over it, because the links *out of* a freed row went
	// with the row. Destroying the middle of three children therefore truncated
	// the list to one, silently, and the two that were left were unreachable.
	//
	// So the unlink happens before the free rather than being repaired
	// afterwards, and "no live row names a freed one" is an invariant instead
	// of something every reader has to tolerate.
	//
	// The children become roots rather than being destroyed. This is not
	// `DestroyInstance` and must not quietly become it: a raw destroy asks for
	// one row to go, and taking a subtree with it would be a delete nobody
	// asked for.
	//
	// @param state    The world holding it.
	// @param instance The instance to unlink. Does nothing for an entity that
	//                 carries no `Hierarchy`.
	void DetachFromTree(StoreState &state, Entity instance);

	// Destroys an instance and everything under it.
	//
	// @param state    The world to remove it from.
	// @param instance The root of the subtree to destroy.
	void DestroyInstance(StoreState &state, Entity instance);

	// One row of a clone, and what it became.
	struct ClonedPair {
		Entity Source;
		Entity Copy;
	};

	// Copies one instance, its components and its whole subtree.
	//
	// **The pairs come back because the caller has to finish the job.** A
	// reference pointing inside the subtree has to be rewritten to point inside
	// the copy of it, and that is a *property*-level operation — the getters
	// and setters take a `Store`, which this layer does not have. So the copy
	// happens here and `Store::CloneInstance` remaps.
	//
	// Anything carrying `NotArchivable` is skipped, itself and its subtree,
	// which is Roblox's `Archivable`.
	//
	// @param state  The world holding the source, which also receives the copy.
	// @param source The instance to copy.
	// @param made   Appended to as `{source, copy}` for every row copied.
	// @return The copy, parented nowhere, or NULL_ENTITY when the source is not
	//         an instance or is not archivable.
	Entity CloneInstance(StoreState &state, Entity source, std::vector<ClonedPair> &made);
}
