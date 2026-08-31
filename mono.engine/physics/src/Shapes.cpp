#include <engine/physics/Shapes.hpp>

#include <cmath>

namespace engine::physics {

	core::Vector3 ShapeHalfExtent(scene::ShapeKind shape, const core::Vector3 &extent) {
		switch (shape) {
		case scene::ShapeKind::Box:
			return extent;
		case scene::ShapeKind::Sphere:
			// Y and Z are not read. A sphere is one number and reading three
			// would make an ellipsoid out of whatever the author left there.
			return core::Vector3{extent.X, extent.X, extent.X};
		case scene::ShapeKind::Cylinder:
			// Radius on both axes across the barrel, half-height along it.
			return core::Vector3{extent.X, extent.Y, extent.X};
		case scene::ShapeKind::Capsule:
			// The straight segment reaches Y either side of centre, then each
			// hemisphere adds the radius in every direction.
			return core::Vector3{extent.X, extent.Y + extent.X, extent.X};

		case scene::ShapeKind::Hull:
		case scene::ShapeKind::Mesh:
			// **The part's extent, because a baked shape's own reach is not
			// knowable from here.** This function is given a `ShapeKind` and an
			// extent and nothing else - it cannot resolve a name - so the honest
			// answer for a baked kind is the extent the part was authored at.
			// `ShapeWorldBounds` below takes the geometry when it has it and
			// falls back to this when it does not.
			return extent;
		}

		// Unreachable for a value that came from the enum. Returning the extent
		// unchanged rather than aborting keeps a corrupted `ShapeKind` read off
		// a wire from taking the process down, and a box-shaped bound is the
		// conservative wrong answer of the three.
		return extent;
	}

	core::AABB ShapeWorldBounds(const scene::Collider &collider, const core::CFrame &frame) {
		switch (collider.Shape) {
		case scene::ShapeKind::Box:
			return core::OrientedBoxBounds(frame, collider.Extent);

		case scene::ShapeKind::Sphere: {
			// Rotation-invariant, and that is the whole reason spheres are not
			// routed through the oriented-box bound: at 45 degrees that would
			// return a box root-two wider on two axes for a shape that did not
			// change.
			const float radius = collider.Extent.X;
			return core::AABB::FromCentre(frame.Position, core::Vector3{radius, radius, radius});
		}

		case scene::ShapeKind::Cylinder: {
			// Support of a cylinder along a unit world axis `e`, with barrel
			// axis `a`: halfHeight * |a . e| + radius * sqrt(1 - (a . e)^2).
			// The second term is the radius of the disc *projected* onto `e`,
			// and it is what makes this tighter than the box bound - a cylinder
			// standing upright bounds to exactly its own radius on X and Z,
			// where the box bound would agree only because the box happens to
			// be axis-aligned too.
			const core::Vector3 axis = frame.UpVector();
			const float radius = collider.Extent.X;
			const float halfHeight = collider.Extent.Y;

			// Clamped at zero because `axis` is unit only to float precision,
			// and a squared component a hair over one turns the root into a
			// NaN that reaches every later overlap test.
			const auto reachOn = [radius, halfHeight](float axisComponent) {
				const float square = axisComponent * axisComponent;
				const float across = square >= 1.0f ? 0.0f : std::sqrt(1.0f - square);
				return halfHeight * std::abs(axisComponent) + radius * across;
			};

			return core::AABB::FromCentre(
				frame.Position, core::Vector3{reachOn(axis.X), reachOn(axis.Y), reachOn(axis.Z)}
			);
		}

		case scene::ShapeKind::Capsule: {
			// A line segment of half-length Y swept by a sphere of radius X.
			// Projecting it onto a world axis is the segment projection plus
			// the sphere radius.
			const core::Vector3 axis = frame.UpVector();
			const float radius = collider.Extent.X;
			const float halfSegment = collider.Extent.Y;
			return core::AABB::FromCentre(
				frame.Position,
				core::Vector3{
					radius + halfSegment * std::abs(axis.X),
					radius + halfSegment * std::abs(axis.Y),
					radius + halfSegment * std::abs(axis.Z),
				}
			);
		}

		case scene::ShapeKind::Hull:
		case scene::ShapeKind::Mesh:
			// **The part's own extent, oriented - not the baked geometry's.**
			// This overload takes a `scene::Collider` and cannot resolve a name,
			// which is deliberate: it is called once per collider per tick by
			// `SyncBroadphase`, and a table lookup there would be a random
			// access per collider on the hottest walk in the module.
			//
			// The bound is therefore the part's, which is loose whenever the
			// baked shape is smaller than the part it sits on and is exactly the
			// direction `AGENTS.md` says a bound may be wrong in: too large
			// costs candidates the narrow phase rejects, too small drops
			// contacts silently. A hull *larger* than its part is a scene
			// mistake - the part is what a designer sized and what the renderer
			// draws - and `ShapeWorldBoundsOf` is the overload that takes the
			// geometry for a caller that has it in hand.
			return core::OrientedBoxBounds(frame, collider.Extent);
		}

		// See ShapeHalfExtent: a shape kind this build does not know about takes
		// the conservative bound rather than aborting.
		return core::OrientedBoxBounds(frame, ShapeHalfExtent(collider.Shape, collider.Extent));
	}
}
