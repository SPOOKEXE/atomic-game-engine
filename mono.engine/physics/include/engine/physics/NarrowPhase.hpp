#pragma once

// Deciding which candidate pairs are really touching, and where.
//
// `Phase::PostSimulation`, after `BroadPhase`. Every pair the broad phase
// admitted is two boxes that overlap; this is the step that intersects the
// shapes inside them and writes a `ContactManifold` for each pair that really
// meets.
//
// **Six exact analytic pairs and no library** - `v02v03v04.md` decision 3.
// Box-box, box-sphere, box-cylinder, sphere-sphere, sphere-cylinder,
// cylinder-cylinder, written in `src/ContactPairs.cpp`. There is no physics
// engine behind this module and there is not going to be one.
//
// **The normal's direction is one convention and it is written down once.**
// A manifold's normal points from `A` toward `B`, `A` is the smaller entity id,
// and the contact points lie on `B`'s surface. `ContactManifold` states it,
// `src/ContactPairs.hpp` states it for the pair functions, and exactly one
// function in the module ever reverses one. Getting it inconsistent between two
// of six pairs is the classic bug in a narrow phase and it reads as objects
// occasionally flying apart rather than as a sign error.
//
// **Box-box produces several points and that is not an optimisation.** A
// single-point manifold cannot hold a resting box still: one point is one
// constraint, the box pivots about it, and the rocking never damps because
// every tick is a fresh single constraint. The cylinder cases are additions to
// the same clipping machinery rather than a second approach.
//
// @tier L8 · shared

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// Intersects every candidate pair and writes the manifolds.
	//
	// `Phase::PostSimulation`, after `BroadPhase` and before `Solve`. Clears
	// `PhysicsWorld::Manifolds` and `PhysicsWorld::Events` and refills the
	// first; the second is `Publish`'s to fill.
	//
	// Manifolds come out in pair order, which is `(min id, max id)` - the
	// solver visits them in that order and sequential impulse is
	// order-dependent, so the ordering is a determinism requirement rather than
	// tidiness.
	//
	// @param store The world to test.
	// @tick
	void NarrowPhase(ecs::Store &store);
}
