#pragma once

// An axis-aligned bounding box, held as two corners.
//
// Two corners rather than a centre and a half-extent because every operation on
// a box is a comparison between corners. Overlap, containment, clamping a point
// onto the surface and the union of two boxes are each one component-wise
// comparison in this form, and a subtraction followed by the same comparison in
// the other. `FromCentre` exists because most callers hold the other form —
// `scene::Bounds` is a local half-extent — and converting once at the boundary
// is cheaper than converting inside every test.
//
// **Touching counts as overlapping.** An exclusive test would separate a
// resting stack for one tick whenever a contact lands exactly on a boundary,
// and a stack that shivers once a second is a bug nobody can reproduce on
// demand.
//
// What is deliberately absent, and why. Each of these is a culling or BVH
// operation with no caller in v0.4, and §3.4 of the design notes is explicit
// that a value type without a caller is a maintenance cost with nothing on the
// other side of the ledger:
//
// - `Inverted()` and an empty sentinel. Nothing here accumulates a box from
//   nothing; `Union` is always given two real boxes.
// - `HalfExtent()`, `Grown()`, `Expanded()`. The one caller that needs a grown
//   box — the swept-box query in `spatial` — builds it with `FromCentre`.
// - `Contains(AABB)`, `Volume()`, `SurfaceArea()`. These are the surface a BVH
//   wants, and decision 4 chose a uniform grid.
// - `Transformed()`. `FromOrientedBox` is the operation that actually exists:
//   the world-space bound of a local box under a rigid transform. A general
//   transform of a box is not a box.
// - Any `Vector2`. It arrives when the overlay or the editor needs one.
//
// **There is no ray/box test here.** The slab test lives privately in
// `spatial/src/`, beside the query whose comment documents its entry, exit and
// normal conventions. It has one caller, and those conventions are not
// properties of a box.
//
// @tier L1 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cmath>

namespace engine::core {

	// A box aligned to the world axes, as its minimum and maximum corners.
	//
	// A valid box has every component of `Minimum` no greater than the matching
	// component of `Maximum`. Nothing here enforces that: a box built the wrong
	// way round overlaps nothing and contains nothing, which is the answer that
	// makes the mistake visible at the first test rather than at the tenth.
	//
	// @since v0.4
	struct AABB {
		// The corner with the smallest component on every axis.
		Vector3 Minimum;

		// The corner with the largest component on every axis.
		Vector3 Maximum;

		// Constructs the degenerate box at the origin, which is a single point.
		constexpr AABB() = default;

		// Constructs a box from its two corners, which are stored as given.
		//
		// @param minimum The smallest corner, in the caller's distance unit.
		// @param maximum The largest corner, in the same unit.
		constexpr AABB(const Vector3 &minimum, const Vector3 &maximum) : Minimum(minimum), Maximum(maximum) {}

		// Constructs a box from its centre and its **half**-extent.
		//
		// Half, because that is the form every containment and overlap test
		// wants and the form `scene::Bounds` stores. Passing a full size here
		// makes every box twice the size it should be, which reads as a physics
		// tuning problem rather than as a units mistake.
		//
		// @param centre     The middle of the box.
		// @param halfExtent How far it reaches from the centre on each axis.
		static constexpr AABB FromCentre(const Vector3 &centre, const Vector3 &halfExtent) {
			return AABB{centre - halfExtent, centre + halfExtent};
		}

		// Returns the world-space box that encloses a rotated local box.
		//
		// The extent grows by the **absolute value** of the rotated half-extent
		// on each axis, which is what makes a unit cube turned 45 degrees about
		// Y come out root two wide. Rotating only the centre and keeping the
		// original extent is a cheaper function that is also wrong: it produces
		// a bound smaller than the shape, and a broad phase whose bound is too
		// small drops contacts without reporting anything.
		//
		// @param frame      Where the box is and how it is turned.
		// @param halfExtent The box's reach from its own centre, in local axes.
		static AABB FromOrientedBox(const CFrame &frame, const Vector3 &halfExtent) {
			const Vector3 right = frame.VectorToWorldSpace(Vector3::XAxis);
			const Vector3 up = frame.VectorToWorldSpace(Vector3::YAxis);
			const Vector3 forward = frame.VectorToWorldSpace(Vector3::ZAxis);

			// Each world axis takes a contribution from all three local axes:
			// this is the absolute value of the rotation matrix applied to the
			// half-extent, written out because the matrix is not stored.
			const Vector3 worldHalfExtent{
				std::abs(right.X) * halfExtent.X + std::abs(up.X) * halfExtent.Y +
					std::abs(forward.X) * halfExtent.Z,
				std::abs(right.Y) * halfExtent.X + std::abs(up.Y) * halfExtent.Y +
					std::abs(forward.Y) * halfExtent.Z,
				std::abs(right.Z) * halfExtent.X + std::abs(up.Z) * halfExtent.Y +
					std::abs(forward.Z) * halfExtent.Z,
			};
			return FromCentre(frame.Position, worldHalfExtent);
		}

		// Returns the middle of the box.
		constexpr Vector3 Centre() const {
			return (Minimum + Maximum) * 0.5f;
		}

		// Returns the full extent on each axis, not the half-extent.
		constexpr Vector3 Size() const {
			return Maximum - Minimum;
		}

		// Reports whether two boxes share any point, counting a shared face.
		//
		// Inclusive on every axis. See the file comment: an exclusive test
		// separates a resting stack for one tick whenever a contact lands on a
		// boundary.
		constexpr bool Overlaps(const AABB &other) const {
			return Minimum.X <= other.Maximum.X && Maximum.X >= other.Minimum.X &&
				   Minimum.Y <= other.Maximum.Y && Maximum.Y >= other.Minimum.Y &&
				   Minimum.Z <= other.Maximum.Z && Maximum.Z >= other.Minimum.Z;
		}

		// Reports whether a point is inside the box, counting the surface.
		//
		// Inclusive for the same reason `Overlaps` is: a point sitting exactly
		// on a face is in contact with it.
		constexpr bool Contains(const Vector3 &point) const {
			return point.X >= Minimum.X && point.X <= Maximum.X && point.Y >= Minimum.Y &&
				   point.Y <= Maximum.Y && point.Z >= Minimum.Z && point.Z <= Maximum.Z;
		}

		// Returns the point of the box nearest to `point`, clamped per axis.
		//
		// A point already inside comes back unchanged. A point outside is
		// clamped onto the face, edge or corner nearest it — never pulled to
		// the centre, which is the shortcut that makes a sphere overlap test
		// pass for spheres that are nowhere near the box.
		constexpr Vector3 ClosestPoint(const Vector3 &point) const {
			// Written out rather than added to Vector3 as a component-wise
			// clamp: Vector3 has consumers across the whole engine and would
			// gain public surface with exactly one caller.
			return Vector3{
				point.X < Minimum.X ? Minimum.X : (point.X > Maximum.X ? Maximum.X : point.X),
				point.Y < Minimum.Y ? Minimum.Y : (point.Y > Maximum.Y ? Maximum.Y : point.Y),
				point.Z < Minimum.Z ? Minimum.Z : (point.Z > Maximum.Z ? Maximum.Z : point.Z),
			};
		}

		// Returns the smallest box containing both this one and `other`.
		constexpr AABB Union(const AABB &other) const {
			return AABB{
				Vector3{
					Minimum.X < other.Minimum.X ? Minimum.X : other.Minimum.X,
					Minimum.Y < other.Minimum.Y ? Minimum.Y : other.Minimum.Y,
					Minimum.Z < other.Minimum.Z ? Minimum.Z : other.Minimum.Z,
				},
				Vector3{
					Maximum.X > other.Maximum.X ? Maximum.X : other.Maximum.X,
					Maximum.Y > other.Maximum.Y ? Maximum.Y : other.Maximum.Y,
					Maximum.Z > other.Maximum.Z ? Maximum.Z : other.Maximum.Z,
				},
			};
		}

		// Reports whether both corners are exactly equal, component by component.
		constexpr bool operator==(const AABB &other) const {
			return Minimum == other.Minimum && Maximum == other.Maximum;
		}
	};
}
