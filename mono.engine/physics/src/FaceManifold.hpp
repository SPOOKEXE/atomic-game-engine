#pragma once

// Turning two facing surface features into contact points.
//
// The separating axis says *which way* two shapes are pressed together and by
// how much. It says nothing about **where**, and where is what decides whether
// a resting box stays still: one point gives the solver one constraint, so the
// box pivots about it and rocks, and the rocking never damps because every tick
// is a fresh single constraint. `v02v03v04.md` §3.5 states it and
// `Contacts.hpp` repeats it beside `MAXIMUM_POINTS`.
//
// So the axis is followed by a clip: take the face each shape presents to the
// other, cut the second against the side planes of the first, and keep the
// pieces that are actually inside. Every pair uses this - a box face against a
// box face, a cylinder cap against a box face, two barrels lying side by side -
// which is what `AGENTS.md` means by the cylinder cases being additions to the
// box-box machinery rather than a second approach.

#include "ShapeSupport.hpp"

#include <engine/core/types/Vector3.hpp>
#include <engine/physics/Contacts.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::physics {

	// What a pair function found, before it becomes a `ContactManifold`.
	//
	// The same fields a manifold carries minus the two entities, because a pair
	// function is given two placed shapes and knows nothing about rows.
	struct ContactSolution {
		// The unit direction separating them, **pointing from the first shape
		// toward the second**. One convention, stated here, obeyed by all six
		// pairs; see `ContactPairs.hpp` for the single place it is flipped.
		core::Vector3 Normal;

		// Where the contacts are, on the surface of the **second** shape.
		core::Vector3 Positions[ContactManifold::MAXIMUM_POINTS];

		// How deep each one is, in metres. Never negative.
		float Penetrations[ContactManifold::MAXIMUM_POINTS] = {};

		// The cache key for each, per `ContactPoint::Feature`.
		uint32_t Features[ContactManifold::MAXIMUM_POINTS] = {};

		// How many of the three arrays above are live.
		uint8_t PointCount = 0;

		// Whether the two shapes touch at all. **False is the answer, not the
		// absence of one** - a pair function that cannot decide says so by
		// leaving this false and writing no points.
		bool Touching = false;
	};

	// How far apart two surfaces may be and still be called a contact.
	//
	// A clipped point sitting a fraction of a millimetre outside is a corner of
	// the same resting face as the three that are inside, and dropping it turns
	// a four-point manifold into a three-point one for one tick - which is a
	// box that twitches at exactly the moment it should be settling. Speculative
	// contacts are the general form of this and are not in scope at v0.4; this
	// is the narrow version that keeps a resting face whole.
	inline constexpr float CONTACT_TOLERANCE = 0.001f;

	// Builds the cache key for one contact point.
	//
	// The two feature ids plus the point's index within the manifold. Nothing
	// reads it as geometry - its whole job is to be the same number next tick
	// for the same physical contact, so the solver finds last tick's impulse.
	//
	// @param first  The first shape's feature id.
	// @param second The second shape's feature id.
	// @param index  Which point of the manifold this is.
	// @return The key.
	constexpr uint32_t ContactFeature(uint8_t first, uint8_t second, size_t index) {
		return (static_cast<uint32_t>(first) << 16) | (static_cast<uint32_t>(second) << 8) |
			   static_cast<uint32_t>(index & 0xFFu);
	}

	// A one-point solution, for the pairs and the degeneracies that have one.
	//
	// @param normal   Unit, first toward second.
	// @param position Where the contact is, on the second shape's surface.
	// @param depth    How deep, in metres.
	// @param feature  The cache key.
	// @return The solution, marked touching.
	ContactSolution
	SinglePoint(const core::Vector3 &normal, const core::Vector3 &position, float depth, uint32_t feature);

	// Builds a manifold from the surfaces two shapes present to each other.
	//
	// The axis and the depth come from whichever test found them; this decides
	// how many points hold the pair apart and where they are.
	//
	// @param first  The shape the normal points away from.
	// @param second The shape the normal points toward.
	// @param normal Unit, first toward second.
	// @param depth  The overlap along `normal`, in metres, and positive.
	// @return The solution, marked touching.
	ContactSolution ManifoldBetween(
		const ShapeInstance &first, const ShapeInstance &second, const core::Vector3 &normal, float depth
	);
}
