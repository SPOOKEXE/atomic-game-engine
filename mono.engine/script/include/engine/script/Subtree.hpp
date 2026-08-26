#pragma once

// Everything under an instance, and what to release when it goes.
//
// **Both VMs destroy the same way, so they destroy through the same walk.**
// `Store::DestroyInstance` takes the whole subtree, so everything the script
// side remembers about that subtree has to be forgotten in step - and the two
// bindings each had their own copy of that walk, each of which stopped at the
// direct children. A grandchild's connections outlived the row they watched.
//
// The walk itself is here rather than in `ecs` because it is only this module
// that needs it: `Store` answers `EachChild` and `EachRoot`, and a descendant
// walk is those composed. If a second module ever wants one, it belongs on
// `Store` and this becomes a caller.
//
// @tier L9 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Changes.hpp>
#include <engine/script/Signals.hpp>

#include <functional>

namespace engine::script {

	// Visits everything under an instance, nearest first.
	//
	// **Depth first, in the order a recursive walk written by hand would
	// produce** - a child, then everything under that child, then the next
	// child. That is Roblox's `GetDescendants` order, and scripts index into
	// the result, so it is a contract rather than an implementation detail.
	//
	// The instance itself is not visited. Iterative rather than recursive: a
	// scene's depth is the author's to choose, and recursion would put it on
	// the C stack.
	//
	// @param store    The world to walk.
	// @param instance The root of the walk, not itself visited.
	// @param body     Called as `body(Entity)` for each descendant.
	void EachDescendant(
		const ecs::Store &store, ecs::Entity instance, const std::function<void(ecs::Entity)> &body
	);

	// Drops every listener on an instance and on everything under it.
	//
	// **Called before `Store::DestroyInstance`, and it must be**: the walk
	// needs the hierarchy the destroy is about to take apart. A connection left
	// behind holds its VM's callable alive for the rest of the world's life and
	// keeps a row nothing can reach in the signal table.
	//
	// Releasing the callable is the caller's, because only the VM that made one
	// knows how - a registry ref on the Luau side, a `JSValue` on the other.
	//
	// @param store    The world the subtree lives in.
	// @param signals  The connection table to drop from.
	// @param changes  The change queue to stop watching in.
	// @param instance The root of the subtree, itself forgotten too.
	// @param release  Called once per callable the table gave up.
	void ForgetSubtree(
		const ecs::Store &store,
		SignalTable &signals,
		ChangeQueue &changes,
		ecs::Entity instance,
		const std::function<void(CallbackRef)> &release
	);
}
