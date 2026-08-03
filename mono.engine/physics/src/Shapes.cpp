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
			return core::AABB::FromOrientedBox(frame, collider.Extent);

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
			// and it is what makes this tighter than the box bound — a cylinder
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
		}

		// See ShapeHalfExtent: a shape kind this build does not know about takes
		// the conservative bound rather than aborting.
		return core::AABB::FromOrientedBox(frame, ShapeHalfExtent(collider.Shape, collider.Extent));
	}
}
