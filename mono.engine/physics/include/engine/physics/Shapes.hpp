#pragma once

// arch-waiver public-header: forward physics API. Bodies and authoring hosts
// exchange this complete collision-shape contract.

// What a `scene::Collider` is, geometrically.
//
// `scene` owns the *data* - `ShapeKind` names the three shapes and `Collider`
// carries an `Extent` - and deliberately owns no arithmetic over it. This file
// is that arithmetic, and it is the only place in the engine that decides what
// `Collider::Extent` means.
//
// **The meaning of `Extent` is written down once, here, and nowhere else.**
//
// | `Shape`    | `Extent.X`            | `Extent.Y`                    | `Extent.Z`            |
// |---|---|---|---|
// | `Box`      | half-extent on local X | half-extent on local Y       | half-extent on local Z |
// | `Sphere`   | radius                 | not read                     | not read              |
// | `Cylinder` | radius                 | half-height along local Y    | not read              |
//
// **Half, never full.** A box a metre across has `Extent` of 0.5 on that axis,
// which is what `scene::Bounds::HalfExtent` stores and what `PartDesc::Size`
// gets halved into. Reading it as a full extent produces a world that is exactly
// twice too big and reads as a physics tuning problem rather than as a units
// mistake - which is why the same rule is stated in the AABB derivation below
// and pinned by a test that fails if the two ever disagree.
//
// **"Not read" means not read.** A sphere's `Extent.Y` and `Extent.Z` are
// whatever the author left there and nothing here looks at them. They are not
// required to equal `Extent.X`, and a caller must not start deriving anything
// from them - a sphere that became an ellipsoid because somebody read three
// components is a change to `ShapeKind`, not to this file.
//
// **Nothing here sanitises a negative extent.** A negative radius produces a box
// whose minimum exceeds its maximum, which `core::AABB` documents as overlapping
// nothing - the answer that makes the mistake visible at the first test rather
// than at the tenth. Clamping would turn an authoring error into a shape that is
// silently the wrong size.
//
// @tier L8 · shared

#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>

namespace engine::physics {

	// One collider, placed in the world.
	//
	// Copied out of `scene::Collider` and `scene::Transform` once per pair
	// rather than held by reference, because a pair function reads the frame
	// eight or ten times and a store lookup per read is the cost an index
	// exists to remove.
	struct ShapeInstance {
		ShapeInstance() = default;

		// The only way in, and deliberately not an aggregate: `Axis` is derived
		// from `Frame` and the two must not be able to disagree.
		ShapeInstance(const core::CFrame &frame, const core::Vector3 &extent, scene::ShapeKind shape);

		// The same, for a shape whose geometry is baked rather than described by
		// an extent.
		//
		// **The kind and the pointer are given together and are checked against
		// each other**, because the failure of getting them apart is silent: a
		// `Hull` with no hull collides as its extent, which is a crate-sized box
		// where a rock should be. The constructor demotes a baked kind with no
		// geometry to `Box`, so a shape whose name did not resolve collides as
		// its bound - see `scene::Collider::Geometry`, which states that as the
		// behaviour rather than as a fallback.
		ShapeInstance(
			const core::CFrame &frame,
			const core::Vector3 &extent,
			scene::ShapeKind shape,
			const collision::ConvexHull *hull,
			const collision::TriangleMesh *mesh
		);

		// Where it is and how it is turned, in world space.
		//
		// **Read-only once built.** Assigning to it leaves `Axis` describing the
		// old rotation; build a new instance instead.
		core::CFrame Frame;

		// Its extent, read according to `Shape`. The table at the top of
		// `Shapes.hpp` is the one definition of what each component means.
		core::Vector3 Extent;

		// The frame's X, Y and Z as world directions, resolved once here.
		//
		// **The whole reason this type is not three plain fields.** `CFrame`
		// holds a quaternion, so every one of these costs a rotation to derive
		// - and every question this header answers is a dot product against one
		// of them. A pair function asks fifteen to twenty-three times over the
		// same two shapes, and deriving them per question made box-box re-rotate
		// the same six vectors ninety times.
		core::Vector3 Axis[3];

		// Which shape `Extent` describes.
		scene::ShapeKind Shape = scene::ShapeKind::Box;

		// The baked geometry, for `ShapeKind::Hull` and `ShapeKind::Mesh`.
		//
		// **Borrowed and never owned.** It points into the world's
		// `scene::CollisionShapes`, which outlives every pair function by a wide
		// margin - a `ShapeInstance` is built inside one step and read inside
		// the same one. A copy would be a hull copied per pair per tick, which
		// is exactly the cost this whole type exists to avoid.
		//
		// **Never both, and never set for the other three kinds.** The
		// constructor is what holds that; a switch on `Shape` is what reads it.
		//@{
		const collision::ConvexHull *Hull = nullptr;
		const collision::TriangleMesh *Mesh = nullptr;
		//@}
	};

	// One collider of a world, placed and resolved, as the broad phase left it.
	//
	// **Filled by `SyncBroadphase` beside the proxy it indexes, and read by the
	// narrow phase by that proxy's own index.** The narrow phase used to resolve
	// a collider once per *pair* it appeared in - about twenty-five thousand
	// `Store::Get` calls for ten thousand colliders - when the sync had already
	// read the same `Transform` and `Collider` for every one of them a few lines
	// earlier. Carrying the answer forward removes every store lookup from the
	// step, which is what lets it be dispatched: measured on ten thousand boxes,
	// the same pair loop went from 89.5 ms of worker time to 3.67 ms once the
	// lookups were out of it.
	//
	// @since v0.17
	struct PlacedCollider {
		// The collider, in world space as of the last sync.
		ShapeInstance Shape;

		// Whether it reports without pushing.
		//
		// Here rather than looked up again, because it is one byte the sync had
		// in hand and a lookup the narrow phase would otherwise have to make.
		bool Trigger = false;

		// Point velocities used by speculative contacts and continuous collision.
		// Static colliders keep both at zero. Carrying them from the broad-phase
		// gather avoids returning to the ECS once per candidate pair.
		//@{
		core::Vector3 LinearVelocity = core::Vector3::Zero;
		core::Vector3 AngularVelocity = core::Vector3::Zero;
		//@}

		// Farthest point from the body's origin, resolved once during sync.
		// Angular speculative reach reads this once per candidate pair, so keeping
		// it beside the shape avoids rebuilding an AABB for every neighbour.
		float MaximumRadius = 0.0f;
	};

	// The local half-extent of the smallest axis-aligned box containing a shape.
	//
	// In the shape's *own* axes, before any rotation. This is the one function
	// that turns the `Extent` table at the top of this file into numbers, so a
	// caller that needs a shape's reach asks here instead of switching on
	// `ShapeKind` again somewhere else.
	//
	// @param shape  Which shape `extent` describes.
	// @param extent The collider's extent, read according to `shape`.
	// @return Half the shape's reach on each local axis.
	core::Vector3 ShapeHalfExtent(scene::ShapeKind shape, const core::Vector3 &extent);

	// The world-space box that just contains a collider placed at `frame`.
	//
	// **Exact per shape, not one oriented-box bound for all three.** A sphere
	// does not grow when it turns, and a cylinder turned 45 degrees is narrower
	// than the box around it - deriving all three from
	// `core::OrientedBoxBounds` would be conservative and therefore correct,
	// and it would also hand the broad phase a sphere 73 per cent too wide and a
	// candidate list to match. The looseness is not free: it is paid once per
	// collider per tick in candidate pairs the narrow phase then rejects.
	//
	// What it must never be is *smaller* than the shape. A broad phase whose
	// bound is too small drops contacts and reports nothing, which is the
	// failure `core::OrientedBoxBounds` was written to avoid and the reason
	// the rotated cases are covered by their own tests.
	//
	// @param collider The shape and its extent.
	// @param frame    Where it is and how it is turned, in world space.
	// @return The collider's world-space bound.
	core::AABB ShapeWorldBounds(const scene::Collider &collider, const core::CFrame &frame);
}
