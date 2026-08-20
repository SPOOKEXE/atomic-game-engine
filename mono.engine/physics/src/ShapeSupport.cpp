#include "ShapeSupport.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

	namespace {
		// How close to zero a perpendicular component may be before the
		// direction counts as parallel to a cylinder's axis.
		//
		// Below it there is no radial direction to build a rim from, and
		// normalising would produce a NaN that reaches every later clip. The
		// fallback is the cylinder's own local X, which is deterministic and
		// exactly as good - a cap seen end-on has no preferred rim vertex.
		constexpr float RADIAL_EPSILON = 1.0e-6f;

		// A cylinder's local X and Z, as world directions perpendicular to its
		// barrel.
		struct RimBasis {
			core::Vector3 First;
			core::Vector3 Second;
		};

		// The rim basis with `First` aligned to `direction`'s radial part.
		//
		// Aligning the first rim vertex with the direction of deepest
		// penetration is what keeps an eight-point octagon from under-reporting
		// depth: the vertex that matters is placed exactly where the disc is
		// deepest rather than up to 22 degrees away from it.
		RimBasis
		BasisFor(const ShapeInstance &shape, const core::Vector3 &axis, const core::Vector3 &direction) {
			const core::Vector3 radial = direction - axis * direction.Dot(axis);
			const core::Vector3 first =
				radial.MagnitudeSquared() > RADIAL_EPSILON ? radial.Unit() : shape.Axis[0];
			return RimBasis{first, axis.Cross(first)};
		}

		// The face of a box whose outward normal is closest to `direction`.
		//
		// `shape.Axis` is right-handed, so `X.Cross(Y) == Z` - which is what the
		// face winding below relies on to point its edge planes inward.
		SupportFeature BoxFace(const ShapeInstance &shape, const core::Vector3 &direction) {
			size_t best = 0;
			float bestAlignment = -1.0f;
			for (size_t index = 0; index < 3; index++) {
				const float alignment = std::abs(direction.Dot(shape.Axis[index]));
				if (alignment > bestAlignment) {
					bestAlignment = alignment;
					best = index;
				}
			}

			const bool positive = direction.Dot(shape.Axis[best]) >= 0.0f;
			const float sign = positive ? 1.0f : -1.0f;
			const core::Vector3 outward = shape.Axis[best] * sign;

			// The two axes the face spans, ordered so `first.Cross(second)` is
			// the outward normal. That is what makes the four points below wind
			// counter-clockwise seen from outside, whichever face was picked.
			const size_t other[3][2] = {{1, 2}, {2, 0}, {0, 1}};
			const size_t firstAxis = positive ? other[best][0] : other[best][1];
			const size_t secondAxis = positive ? other[best][1] : other[best][0];

			const float extent[3] = {shape.Extent.X, shape.Extent.Y, shape.Extent.Z};
			const core::Vector3 first = shape.Axis[firstAxis] * extent[firstAxis];
			const core::Vector3 second = shape.Axis[secondAxis] * extent[secondAxis];
			const core::Vector3 centre = shape.Frame.Position + outward * extent[best];

			SupportFeature face;
			face.Plane = outward;
			face.Count = 4;
			face.Points[0] = centre - first - second;
			face.Points[1] = centre + first - second;
			face.Points[2] = centre + first + second;
			face.Points[3] = centre - first + second;
			face.Id = static_cast<uint8_t>(best * 2 + (positive ? 0 : 1));
			return face;
		}

		// The cap or the barrel strip of a cylinder, whichever faces
		// `direction`.
		//
		// The split is on which component of the direction is larger, the
		// axial one or the radial one. **Both branches are contact cases the
		// design names**: the cap is a cylinder standing on its end, the strip
		// is one lying on its side, and the boundary between them is the
		// disc-edge case that has a test of its own.
		SupportFeature CylinderFeature(const ShapeInstance &shape, const core::Vector3 &direction) {
			const core::Vector3 axis = BarrelAxis(shape);
			const float radius = shape.Extent.X;
			const float halfHeight = shape.Extent.Y;

			const float along = direction.Dot(axis);
			const core::Vector3 radial = direction - axis * along;
			const float across = radial.Magnitude();

			SupportFeature feature;

			if (across > std::abs(along)) {
				// Lying on its side: the contact is the line where the barrel
				// touches, which is two points and never one. A single point
				// here is a cylinder that rolls on its side while resting.
				const core::Vector3 outward = across > RADIAL_EPSILON ? radial / across : shape.Axis[0];
				const core::Vector3 centre = shape.Frame.Position + outward * radius;
				feature.Plane = outward;
				feature.Count = 2;
				feature.Points[0] = centre - axis * halfHeight;
				feature.Points[1] = centre + axis * halfHeight;
				feature.Id = 2;
				return feature;
			}

			const float sign = along >= 0.0f ? 1.0f : -1.0f;
			const core::Vector3 outward = axis * sign;
			const core::Vector3 centre = shape.Frame.Position + outward * halfHeight;
			const RimBasis basis = BasisFor(shape, axis, direction);

			// Wound about `outward`, which flips with the cap: the first basis
			// vector crossed into the second gives +axis, so the bottom cap has
			// to be walked the other way round or every edge plane points out.
			feature.Plane = outward;
			feature.Count = MAXIMUM_FEATURE_POINTS;
			for (size_t index = 0; index < MAXIMUM_FEATURE_POINTS; index++) {
				const auto step = static_cast<float>(index);
				const float turn = step * (6.2831853072f / static_cast<float>(MAXIMUM_FEATURE_POINTS)) * sign;
				const core::Vector3 offset =
					basis.First * (std::cos(turn) * radius) + basis.Second * (std::sin(turn) * radius);
				feature.Points[index] = centre + offset;
			}
			feature.Id = static_cast<uint8_t>(sign > 0.0f ? 0 : 1);
			return feature;
		}
	}

	ShapeInstance::ShapeInstance(
		const core::CFrame &frame, const core::Vector3 &extent, scene::ShapeKind shape
	)
		: ShapeInstance(frame, extent, shape, nullptr, nullptr) {}

	ShapeInstance::ShapeInstance(
		const core::CFrame &frame,
		const core::Vector3 &extent,
		scene::ShapeKind shape,
		const collision::ConvexHull *hull,
		const collision::TriangleMesh *mesh
	)
		: Frame(frame), Extent(extent),
		  Axis{frame.RightVector(), frame.UpVector(), frame.VectorToWorldSpace(core::Vector3::ZAxis)},
		  Shape(shape), Hull(shape == scene::ShapeKind::Hull ? hull : nullptr),
		  Mesh(shape == scene::ShapeKind::Mesh ? mesh : nullptr) {
		// **A baked kind with nothing baked collides as its extent.** The name
		// did not resolve - content still streaming, a typo, a world saved
		// against a mesh that has since gone - and the two answers available are
		// a box the size of the part or no collision at all. A part that
		// silently stops colliding is a floor that is not there; a box is
		// visibly the wrong shape and stops things. `scene::Collider::Geometry`
		// states this as the behaviour rather than as a fallback.
		if ((Shape == scene::ShapeKind::Hull && Hull == nullptr) ||
			(Shape == scene::ShapeKind::Mesh && Mesh == nullptr)) {
			Shape = scene::ShapeKind::Box;
		}
	}

	core::Vector3 ToLocalPoint(const core::CFrame &frame, const core::Vector3 &point) {
		return ToLocalVector(frame, point - frame.Position);
	}

	core::Vector3 ToLocalVector(const core::CFrame &frame, const core::Vector3 &vector) {
		const glm::vec3 local = glm::conjugate(frame.Rotation()) * glm::vec3{vector.X, vector.Y, vector.Z};
		return core::Vector3{local.x, local.y, local.z};
	}

	float ProjectionRadius(const ShapeInstance &shape, const core::Vector3 &axis) {
		// **A hull and a mesh never reach here, and that is a routing rule
		// rather than an omission.** The expression this feeds -
		// `radiusA + radiusB - |offset . axis|` - is exact *because* all three
		// analytic shapes are centrally symmetric, which the file comment states
		// at length. A convex hull is not: its reach along an axis and its reach
		// along the opposite one are different numbers, so a single radius about
		// the frame's origin is not a wrong-but-conservative answer, it is a
		// wrong answer in both directions at once.
		//
		// `ContactBetween` sends every pair naming one to `ConvexQuery` before
		// any axis search happens. The cases below exist so the compiler still
		// checks this switch is exhaustive; reaching one is a routing bug, and
		// the half-extent it returns is the conservative reading of a shape
		// nothing has resolved.
		switch (shape.Shape) {
		case scene::ShapeKind::Box:
			return std::abs(axis.Dot(shape.Axis[0])) * shape.Extent.X +
				   std::abs(axis.Dot(shape.Axis[1])) * shape.Extent.Y +
				   std::abs(axis.Dot(shape.Axis[2])) * shape.Extent.Z;

		case scene::ShapeKind::Sphere:
			// Rotation-invariant, which is the whole reason a sphere is one
			// number: Y and Z of `Extent` are not read anywhere.
			return shape.Extent.X;

		case scene::ShapeKind::Cylinder: {
			// Half-height along the barrel plus the radius of the disc
			// projected across it. Clamped at zero because the axis is unit
			// only to float precision, and a squared component a hair over one
			// turns the root into a NaN that reaches every later test.
			const float along = axis.Dot(BarrelAxis(shape));
			const float square = along * along;
			const float across = square >= 1.0f ? 0.0f : std::sqrt(1.0f - square);
			return shape.Extent.Y * std::abs(along) + shape.Extent.X * across;
		}

		case scene::ShapeKind::Hull:
		case scene::ShapeKind::Mesh:
			// Unreachable; see the paragraph above this switch. Zero reach means
			// a shape that separates from everything rather than one that
			// produces a contact with a made-up depth, which is the direction a
			// routing bug should fail in.
			return 0.0f;
		}

		// Unreachable for a value that came from the enum. A `ShapeKind` read
		// off a wire and corrupted gets zero reach, so it separates from
		// everything rather than producing a contact with a made-up depth -
		// the same instinct as `ShapeWorldBounds`, aimed the other way because
		// here the conservative answer is "no contact" rather than "a bigger
		// box".
		return 0.0f;
	}

	core::Vector3 SupportPoint(const ShapeInstance &shape, const core::Vector3 &direction) {
		switch (shape.Shape) {
		case scene::ShapeKind::Hull: {
			// **The direction goes into the hull's space and the answer comes
			// back out**, which is two rotations rather than rotating every one
			// of the hull's points into the world. A hull is up to
			// `collision::MAXIMUM_HULL_POINTS` corners and this is asked several
			// times per contact per iteration, so the difference is two
			// quaternion products against sixty-four.
			const core::Vector3 local = ToLocalVector(shape.Frame, direction);
			const core::Vector3 point = collision::SupportPoint(*shape.Hull, local);
			return shape.Frame.Position + shape.Frame.VectorToWorldSpace(point);
		}

		case scene::ShapeKind::Mesh:
			// **A triangle soup has no support point worth the name.** It is a
			// surface rather than a solid, so the furthest point of it along a
			// direction is a corner of whichever triangle happens to stick out -
			// which is not the answer any convex algorithm is asking for.
			// `ContactBetween` solves a mesh triangle by triangle, and each
			// triangle is a three-point hull with a support of its own.
			return shape.Frame.Position;

		case scene::ShapeKind::Box: {
			const float extent[3] = {shape.Extent.X, shape.Extent.Y, shape.Extent.Z};
			core::Vector3 point = shape.Frame.Position;
			for (size_t index = 0; index < 3; index++) {
				const float sign = direction.Dot(shape.Axis[index]) >= 0.0f ? 1.0f : -1.0f;
				point = point + shape.Axis[index] * (extent[index] * sign);
			}
			return point;
		}

		case scene::ShapeKind::Sphere:
			return shape.Frame.Position + direction * shape.Extent.X;

		case scene::ShapeKind::Cylinder: {
			const core::Vector3 axis = BarrelAxis(shape);
			const float along = direction.Dot(axis);
			const core::Vector3 radial = direction - axis * along;
			const float across = radial.Magnitude();
			const core::Vector3 outward = across > RADIAL_EPSILON ? radial / across : core::Vector3::Zero;
			const float sign = along >= 0.0f ? 1.0f : -1.0f;
			return shape.Frame.Position + axis * (shape.Extent.Y * sign) + outward * shape.Extent.X;
		}
		}

		return shape.Frame.Position;
	}

	SupportFeature FaceTowards(const ShapeInstance &shape, const core::Vector3 &direction) {
		switch (shape.Shape) {
		case scene::ShapeKind::Hull: {
			// The face whose outward normal is most nearly the direction asked
			// for, which is what a clip needs: the widest flat piece of the
			// surface facing that way. A hull with no faces - flat, or a line,
			// or fewer than four distinct points, all of which
			// `collision::BuildConvexHull` produces deliberately - has no face
			// to present, and the support *point* is the honest answer instead.
			const core::Vector3 local = ToLocalVector(shape.Frame, direction);

			size_t best = shape.Hull->Faces.size();
			float facing = -2.0f;
			for (size_t index = 0; index < shape.Hull->Faces.size(); index++) {
				const float towards = shape.Hull->Faces[index].Normal.Dot(local);
				if (towards > facing) {
					facing = towards;
					best = index;
				}
			}

			SupportFeature feature;
			if (best == shape.Hull->Faces.size()) {
				feature.Points[0] = SupportPoint(shape, direction);
				feature.Plane = direction;
				feature.Count = 1;
				return feature;
			}

			const collision::HullFace &face = shape.Hull->Faces[best];
			feature.Plane = shape.Frame.VectorToWorldSpace(face.Normal);

			// **Capped at `MAXIMUM_FEATURE_POINTS`, which is a clip budget and
			// not a hull limit.** A baked hull may have a twenty-sided face and
			// the clipper takes eight points; keeping the first eight of the
			// loop is a convex sub-polygon of the face, which is the same kind
			// of approximation an inscribed octagon is for a cylinder cap.
			const uint32_t taken =
				std::min<uint32_t>(face.IndexCount, static_cast<uint32_t>(MAXIMUM_FEATURE_POINTS));
			for (uint32_t at = 0; at < taken; at++) {
				const core::Vector3 &corner = shape.Hull->Points[shape.Hull->Loops[face.FirstIndex + at]];
				feature.Points[at] = shape.Frame.Position + shape.Frame.VectorToWorldSpace(corner);
			}
			feature.Count = taken;

			// **The face index is the feature id**, which is what the warm start
			// keys on. A hull's faces keep their order between builds of the
			// same points - `collision/AGENTS.md` makes that a requirement - so
			// the same physical contact carries the same number next tick.
			feature.Id = static_cast<uint8_t>(best & 0xFFu);
			return feature;
		}

		case scene::ShapeKind::Mesh: {
			// A soup presents no face; see `SupportPoint`.
			SupportFeature feature;
			feature.Points[0] = shape.Frame.Position;
			feature.Plane = direction;
			feature.Count = 1;
			return feature;
		}

		case scene::ShapeKind::Box:
			return BoxFace(shape, direction);

		case scene::ShapeKind::Sphere: {
			// One point, and that is not a limitation to work around. A sphere
			// touches a plane at exactly one place, so a manifold with more
			// points would be inventing constraints - and a ball resting on a
			// floor is *supposed* to be free to roll.
			SupportFeature feature;
			feature.Plane = direction;
			feature.Count = 1;
			feature.Points[0] = shape.Frame.Position + direction * shape.Extent.X;
			feature.Id = 0;
			return feature;
		}

		case scene::ShapeKind::Cylinder:
			return CylinderFeature(shape, direction);
		}

		SupportFeature feature;
		feature.Plane = direction;
		feature.Count = 1;
		feature.Points[0] = shape.Frame.Position;
		return feature;
	}
}
