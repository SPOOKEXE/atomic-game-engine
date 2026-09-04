#pragma once

// arch-waiver public-header: forward physics API. Collision hosts use this
// complete narrow-phase contract without duplicating pair policy.

// Deciding which candidate pairs are really touching, and where.
//
// `Phase::PostSimulation`, after `BroadPhase`. Every pair the broad phase
// admitted is two boxes that overlap; this is the step that intersects the
// shapes inside them and writes a `ContactManifold` for each pair that really
// meets.
//
// **Six exact analytic pairs and no library.**
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

	// How many candidate pairs one worker takes at a time.
	//
	// **Chosen against the shape of the body**, which `parallel/AGENTS.md`
	// allows when the work is plainly one side of the crossover: one pair is two
	// array subscripts and an exact shape test, and the most expensive of the
	// six pairs - box against cylinder - is 375 nanoseconds on its own. That is
	// hundreds of times `Jobs::DEFAULT_GRAIN`'s assumption about a cheap index.
	//
	// **It could not be dispatched at all until the lookups left.** The body was
	// two `Store::Get` calls per pair before v0.17, and twenty-four workers
	// doing that contend rather than share - see the note in `NarrowPhase.cpp`,
	// which carries the two measurements that led to the pairs carrying their
	// proxy indices instead.
	//
	// @since v0.17
	inline constexpr size_t NARROW_GRAIN = 256;

	// Extra separation retained around a predicted contact.
	//
	// Two millimetres, four times the solver's penetration slop. This is a
	// numerical skin rather than long-distance prediction, which belongs to the
	// continuous collision pass. A larger margin creates more exact distance
	// work and more chances for a body to feel a surface it never reaches.
	inline constexpr float SPECULATIVE_DISTANCE = 0.002f;

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
