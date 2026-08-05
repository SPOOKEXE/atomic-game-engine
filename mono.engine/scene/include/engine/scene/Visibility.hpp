#pragma once

// What is actually drawn: the `Workspace` subtree, and only it.
//
// **The tree decides what renders, and before this nothing did.** Every query
// that built a draw list matched on `<Transform, PreviousTransform, Bounds,
// Visual>` — a *component* test — so an entity was drawn because of what it was
// made of and never because of where it sat. A part in `ReplicatedStorage`, a
// template under `StarterGui`, an orphan a script had created and not yet
// parented: all of them were complete parts by that test, and all of them were
// on screen. `Visual::Visible` was declared, bound, saved and reloaded, and no
// draw path read it either.
//
// Roblox's rule is the one authors expect and the one this restores: a
// `BasePart` is rendered exactly when it is a descendant of `Workspace`.
// Everything else in the tree is storage.
//
// ## Why a tag component and not a boolean test
//
// The obvious fix is a branch in the collect loop, and it is the wrong one
// twice over.
//
// The first reason is mechanical. `client::CollectInstances` runs
// `EachBatchParallel` and writes `out[first + row]` — each worker is told where
// its slice lands so no two touch the same bytes and no atomic is needed.
// A `continue` inside that loop leaves holes in the output and makes the
// returned count a lie; compacting instead would need a parallel prefix sum
// over the batches, which is a great deal of machinery to avoid work the
// storage can avoid for free.
//
// The second is the ECS's own argument, made in `ecs/AGENTS.md` and again by
// `RigidBody`: **componentise what you iterate.** An anchored part carries no
// `Motion`, so it lands in a different archetype and the dynamic queries never
// visit it — which beats testing a boolean per row per tick. This is the same
// shape. A hidden part carries no `Rendered`, so the draw query never visits
// it, and the cost of a scene where nine tenths of the content is in storage is
// the tenth that is on screen.
//
// ## Why a scan, and not maintenance at the point of the write
//
// Keeping the tag in step incrementally means hooking every path that can
// change the answer, and there are more of them than there look:
// `Store::SetParent` from the `Parent` property, a studio drag in the explorer,
// a game file loading, `InstallServices`, `DestroyInstance` taking a subtree,
// and any C++ that reparents directly. Missing one gives a part that is in
// `Workspace` and invisible, or worse a part that was deleted from it and still
// draws — and the symptom appears in the renderer, a very long way from the
// reparent that caused it.
//
// Worse, ancestry is not local: parenting one model moves every part beneath
// it, so even a complete set of hooks has to walk a subtree.
//
// So this walks instead, once per tick, and is correct regardless of who moved
// what. It is O(descendants of Workspace) plus O(currently rendered) — both
// linear in the size of the scene, neither allocating after the first call —
// and it is behind a profile scope rather than an assurance, because this
// codebase's habit is to make a cost measurable rather than argue about it.
//
// ## Where it runs, and the one sharp edge in it
//
// `PreRender`, immediately before the system that builds the draw list. This is
// presentation state and that is the phase that derives presentation state.
//
// **It is the one thing in `PreRender` that is structural**, and that deserves
// naming rather than hiding: adding or removing a component moves the row to
// another archetype, and `PreRender` runs at the display's rate rather than the
// tick's. Two things make it safe. Nothing in the simulation reads `Rendered` —
// it exists to be a term in the draw query and for nothing else — so a row
// changing table cannot change what a tick computes. And every host derives it
// the same way from the same tree, so two runs of one scene still agree, which
// is what `just determinism` actually compares.
//
// The alternative was `PostSimulation`, once per tick, and it is wrong for a
// reason that is easy to miss until something is broken by it: **a world can
// present without ticking.** The studio suspends a world and edits it, so a
// gate maintained by the simulation would leave a part dragged into `Workspace`
// invisible until somebody pressed play — and any future host that calls
// `Present` without `Tick` would render an empty scene with nothing to say why.
// A phase that only works for worlds that tick is a trap laid for the next
// caller.
//
// ## What a replica does instead
//
// **A replicated world does not use this at all**, and the reason is worth
// stating here rather than only at the call site that skips it. This is an
// *ancestry* test, and ancestry is the one thing the wire does not carry: only
// `Transform`, `Motion`, `Bounds` and `Visual` are replicated, and `Hierarchy`
// holds `Entity` handles that mean nothing until they are remapped between two
// processes' directories.
//
// A replica does not need one either. The authority replicates what is in its
// own scene, so the wire has already applied this filter — and
// `client::CollectReplicated` honours `Visual::Visible` directly, because that
// one does arrive.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Present exactly on the entities a draw list should contain.
	//
	// **Maintained, never authored.** Nothing outside `SyncRendered` may add or
	// remove one: it is a derived fact about the tree, and a second writer would
	// be a second opinion about what is on screen. It is deliberately absent
	// from every class's component set for the same reason — `Instance.new`
	// must not mint a part that is already claiming to be drawn.
	//
	// Carries a mark rather than being empty so that the sweep can find what has
	// gone stale without a side table. See `SyncRendered`.
	//
	// @since v0.7
	struct Rendered {
		// Set by the walk, cleared by the sweep, and therefore **always zero
		// between passes**.
		//
		// Mark-and-sweep, and the alternative was a `std::set` of every entity
		// the walk visited — allocated, hashed and thrown away once per tick.
		// This is a byte in a row the sweep is already touching.
		//
		// **A mark and not a monotonic stamp, because this component reaches a
		// snapshot.** A counter would be a number that differs between a live
		// run and a replay of it, written into the file both are compared by;
		// a field that is zero at every point anything can observe cannot be.
		//
		// Explicitly `uint8_t` and padded, for the reason every other
		// `Reserved` in the engine exists.
		uint8_t Mark = 0;

		// Explicit padding, so the object representation a snapshot writes
		// holds no uninitialised bytes.
		uint8_t Reserved[3] = {};
	};

	// Brings `Rendered` in step with the tree.
	//
	// Adds the tag to every descendant of `Workspace` that carries a `Visual`
	// with `Visible` set, and removes it from everything else. Idempotent: a
	// second call on an unchanged world adds and removes nothing.
	//
	// **A world with no `Workspace` renders nothing**, rather than everything.
	// That is the safe direction: the failure of a world that has not had
	// `InstallServices` run on it should be an empty screen somebody
	// investigates, not a scene that draws its own storage.
	//
	// Structural changes made from here are deferred by the store when it is
	// called from inside iteration, exactly as any other `Set` or `Remove` is —
	// but it is meant to be called between systems, where they apply at once.
	//
	// @param store The world to bring in step.
	// @return How many entities carry `Rendered` when this returns.
	size_t SyncRendered(ecs::Store &store);
}
