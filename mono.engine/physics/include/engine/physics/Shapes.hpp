#pragma once

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

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>

namespace engine::physics {

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
	// `core::AABB::FromOrientedBox` would be conservative and therefore correct,
	// and it would also hand the broad phase a sphere 73 per cent too wide and a
	// candidate list to match. The looseness is not free: it is paid once per
	// collider per tick in candidate pairs the narrow phase then rejects.
	//
	// What it must never be is *smaller* than the shape. A broad phase whose
	// bound is too small drops contacts and reports nothing, which is the
	// failure `core::AABB::FromOrientedBox` was written to avoid and the reason
	// the rotated cases are covered by their own tests.
	//
	// @param collider The shape and its extent.
	// @param frame    Where it is and how it is turned, in world space.
	// @return The collider's world-space bound.
	core::AABB ShapeWorldBounds(const scene::Collider &collider, const core::CFrame &frame);
}
