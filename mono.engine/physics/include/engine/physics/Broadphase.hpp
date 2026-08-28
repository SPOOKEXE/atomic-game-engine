#pragma once

// Finding which colliders might touch, and nothing about whether they do.
//
// Two steps, and they are separate because they run in different phases. The
// world's boxes are put into the index while the simulation is still moving
// things; the pairs are read out of it once nothing else will move. Fusing them
// would mean a pair list built from positions that a later `Simulation` system
// invalidated.
//
// **Both steps are exact about what they are not.** A candidate pair is two
// boxes that overlap and two layer masks that admit each other. It is not a
// contact, there is no normal, and there is no depth - those are `NarrowPhase`,
// which runs next and throws most of these away.
//
// @tier L8 · shared

#include <engine/physics/PhysicsWorld.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Whether the layer masks allow these two colliders to be paired.
	//
	// **Both directions, and the choice is not arbitrary.** A pair is admitted
	// only when each side's layer is in the other's mask -
	// `a.Mask ∩ b.Layer` *and* `b.Mask ∩ a.Layer`. `scene::Collider::Mask`
	// already states this in its own comment, so the alternative reading -
	// either direction is enough - would make the component's documentation
	// false rather than merely making a different choice.
	//
	// The consequence a caller has to hold: **filtering is symmetric even though
	// the masks are not.** Setting A's mask to see B does not make the pair
	// happen; B's mask has to see A as well. That is the conservative half of
	// the two, and it is the one that cannot produce a contact nobody asked for.
	//
	// @param a One collider's record.
	// @param b The other's.
	// @return `true` when both masks admit the other's layer.
	constexpr bool PairAdmitted(const ColliderRecord &a, const ColliderRecord &b) {
		return a.Mask.Overlaps(b.Layer) && b.Mask.Overlaps(a.Layer);
	}

	// Puts every collider's world box into the world's indexes.
	//
	// `Phase::Simulation`, after `IntegrateMotion`, because the boxes have to
	// describe where things ended the tick rather than where they started it.
	//
	// **The box comes from `scene::Collider`, not from `scene::Bounds`.** The
	// two are the same number for a `MakePart` box and need not stay that way:
	// `Bounds` is the extent a thing is *drawn* at and `Collider::Extent` is the
	// shape it *collides* as, and an index built from the first can be smaller
	// than the shape in the second. A broad phase whose bound is too small drops
	// contacts and reports nothing - the exact failure
	// `core::OrientedBoxBounds` was written to avoid. The original plan named
	// `Bounds` here; that predates `Collider` carrying its own extent, and
	// `AGENTS.md` in this directory records the departure.
	//
	// **Dynamic and static are two sets and two rebuilds.** A collider with a
	// `scene::Motion` can move and is re-measured every tick. One without cannot
	// - `MakePart` gives an anchored part neither `RigidBody` nor `Motion`, so
	// static geometry is already its own archetype - and its index is rebuilt
	// only when the static set changes. Staleness comes from `ecs::ChangeChannel`
	// stamps on `scene::Transform` and `scene::Collider`, which
	// `PreparePhysicsWorld` starts observing when the world is built.
	//
	// @param store The world to index.
	// @tick
	void SyncBroadphase(ecs::Store &store);

	// Reads the candidate pairs out of the indexes.
	//
	// `Phase::PostSimulation`. Writes `PhysicsWorld::Pairs`, **sorted by
	// `(min id, max id)` and deduplicated**. A pair is never reported twice and
	// nothing is ever paired with itself.
	//
	// Only dynamic colliders are queried, so two static colliders never form a
	// pair. Two pieces of anchored geometry that overlap are the level author's
	// business and cost the solver nothing to ignore.
	//
	// @param store The world to read.
	// @tick
	void BroadPhase(ecs::Store &store);
}
