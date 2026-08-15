#pragma once

// What a touch is.
//
// **These types were written before the narrow phase that fills them in**, for
// one reason: the shape of a manifold decides whether the solver can hold a box
// still, and getting it wrong is a rewrite of the narrow phase, the solver, the
// contact cache and the event surface together rather than an edit to one. They
// have a producer now - `NarrowPhase` and `Publish` - and the decisions below
// are what that producer had to be built against.
//
// **A manifold holds several points, and that is the load-bearing decision.**
// `v02v03v04.md` §3.5 is explicit: a single-point manifold cannot hold a resting
// box still. One point gives the solver one constraint, so a box on a floor
// pivots about that point and rocks - every frame the contact moves, and the
// rocking never damps out because each frame is a fresh single constraint.
// Designing for one point now and adding the rest later means rewriting the
// solver, the cache key and the event surface together.
//
// **The normal's direction is a convention and it is stated once.** From `A`
// toward `B`, with `A` the smaller entity id and the points on `B`'s surface.
// Six pair functions obey it and exactly one function in the module reverses
// one - `src/ContactPairs.hpp` is where that is written down. Two of six
// disagreeing reads as objects occasionally flying apart rather than as a sign
// error, which is why it is a convention rather than a per-pair choice.
//
// @tier L8 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::physics {

	// One place two colliders touch.
	//
	// @since v0.4
	struct ContactPoint {
		// Where the contact is, in world space, on the surface of `B`.
		core::Vector3 Position;

		// How far the two shapes overlap along the manifold's normal, in
		// metres. Never negative: a separated pair is not a contact, and a
		// negative depth here would be a separated pair the solver then pushed
		// together.
		float Penetration = 0.0f;

		// Which features of the two shapes met - a face, an edge, a vertex,
		// combined into one number by whatever produces the manifold.
		//
		// **A key, not a description.** Its only job is to be the same number
		// next tick for the same physical contact, so a persistent cache can
		// find last tick's impulse and warm-start from it. Nothing reads it as
		// geometry, and its encoding is the narrow phase's to choose.
		uint32_t Feature = 0;
	};

	// Every point at which one pair of colliders touches, with the one normal
	// they share.
	//
	// One normal for the whole manifold rather than one per point, because a
	// solver iterating a face contact has to push along a single direction -
	// per-point normals turn a resting box into four independent constraints
	// pulling four ways, which is the jitter the multi-point manifold exists to
	// remove.
	//
	// @since v0.4
	struct ContactManifold {
		// How many points one manifold may hold.
		//
		// Four. A face-on-face contact between two boxes clips to a polygon with
		// up to eight vertices, and every solver worth copying reduces that to
		// the four that best preserve the contact area - four points hold a box
		// against translation and both rotations, and the fifth adds cost
		// without adding a constraint the first four did not already imply.
		// Reduction is the narrow phase's job; this is the budget it reduces to.
		static constexpr size_t MAXIMUM_POINTS = 4;

		// The first collider, always the one with the smaller entity id - the
		// same ordering `CandidatePair` uses, so a contact and the pair it came
		// from name their two bodies the same way round.
		ecs::Entity A;

		// The second collider, always the one with the larger entity id.
		ecs::Entity B;

		// The unit direction that separates them, pointing from `A` toward `B`.
		core::Vector3 Normal;

		// The contact points, of which the first `PointCount` are live.
		ContactPoint Points[MAXIMUM_POINTS];

		// How many of `Points` the narrow phase filled in.
		uint8_t PointCount = 0;

		// Whether either collider is a trigger, so this manifold is reported and
		// never solved.
		bool Trigger = false;
	};

	// What happened to a pair between one tick and the next.
	//
	// @since v0.4
	enum class ContactPhase : uint8_t {
		// The two were not touching last tick and are now.
		Began,

		// They were touching last tick and still are.
		Persisted,

		// They were touching last tick and are not now.
		Ended,
	};

	// One pair's change of contact state, for game code that reacts to touching.
	//
	// Deliberately not a manifold. A script asking "did these two touch" wants
	// two entities and an edge, and handing it four points and a normal makes
	// every listener depend on the narrow phase's output format - so an event is
	// the pair and the transition, and a listener that needs the geometry reads
	// the manifold list for the same pair.
	//
	// @since v0.4
	struct ContactEvent {
		// The collider with the smaller entity id.
		ecs::Entity A;

		// The collider with the larger entity id.
		ecs::Entity B;

		// What changed.
		ContactPhase Phase = ContactPhase::Began;
	};
}
