#include "ShapeRay.hpp"

#include "ShapeSupport.hpp"

#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

namespace engine::physics {

	namespace {
		// Below this a direction component counts as parallel to the pair of
		// planes it would otherwise cross. The same number and the same reason
		// as `spatial/src/RayBox.hpp`: far below anything a unit direction can
		// hold on three axes at once, and well clear of where a reciprocal
		// becomes an infinity that later multiplies a zero into a NaN.
		constexpr float PARALLEL_EPSILON = 1.0e-20f;

		// A one-dimensional interval along the ray.
		struct Interval {
			float Entry = -std::numeric_limits<float>::infinity();
			float Exit = std::numeric_limits<float>::infinity();
			bool Empty = false;
		};

		ShapeHit BoxRay(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
			// **In the box's own axes.** This inverse transform is the whole of
			// the rotated-box case: every axis-aligned test passes whether or
			// not it is here, which is why `v02v03v04.md` §3.7 asks for a
			// raycast against a rotated box by name.
			const core::Vector3 origin = ToLocalPoint(shape.Frame, ray.Origin);
			const core::Vector3 direction = ToLocalVector(shape.Frame, ray.Direction);

			const float start[3] = {origin.X, origin.Y, origin.Z};
			const float step[3] = {direction.X, direction.Y, direction.Z};
			const float extent[3] = {shape.Extent.X, shape.Extent.Y, shape.Extent.Z};

			float entry = -std::numeric_limits<float>::infinity();
			float exit = std::numeric_limits<float>::infinity();
			int entryAxis = -1;

			for (int axis = 0; axis < 3; axis++) {
				if (std::abs(step[axis]) < PARALLEL_EPSILON) {
					// Answered without arithmetic, because the numerator can be
					// zero too and zero times infinity is a NaN that compares
					// false in both directions - the miss goes undetected and
					// so does the hit.
					if (start[axis] < -extent[axis] || start[axis] > extent[axis]) {
						return ShapeHit{};
					}
					continue;
				}

				const float inverse = 1.0f / step[axis];
				float near = (-extent[axis] - start[axis]) * inverse;
				float far = (extent[axis] - start[axis]) * inverse;
				if (near > far) {
					const float swapped = near;
					near = far;
					far = swapped;
				}
				if (near > entry) {
					entry = near;
					entryAxis = axis;
				}
				if (far < exit) {
					exit = far;
				}
			}

			if (entryAxis < 0 || entry > exit || exit < 0.0f || entry > maxDistance) {
				return ShapeHit{};
			}

			const float sign = step[entryAxis] > 0.0f ? -1.0f : 1.0f;
			const core::Vector3 local{
				entryAxis == 0 ? sign : 0.0f,
				entryAxis == 1 ? sign : 0.0f,
				entryAxis == 2 ? sign : 0.0f,
			};
			return ShapeHit{true, entry > 0.0f ? entry : 0.0f, shape.Frame.VectorToWorldSpace(local)};
		}

		ShapeHit SphereRay(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
			const float radius = shape.Extent.X;
			const core::Vector3 offset = ray.Origin - shape.Frame.Position;
			const float along = offset.Dot(ray.Direction);
			const float gap = offset.MagnitudeSquared() - radius * radius;

			// Inside, so the entry is here. `RayBox.hpp`'s convention: a query
			// asking what it is touching wants "here", not the far wall.
			if (gap <= 0.0f) {
				const core::Vector3 outward =
					offset.MagnitudeSquared() > 0.0f ? offset.Unit() : -ray.Direction;
				return ShapeHit{true, 0.0f, outward};
			}

			// Pointing away and already outside, so nothing ahead can be
			// nearer than the origin already is.
			if (along > 0.0f) {
				return ShapeHit{};
			}

			const float discriminant = along * along - gap;
			if (discriminant < 0.0f) {
				return ShapeHit{};
			}

			const float distance = -along - std::sqrt(discriminant);
			if (distance > maxDistance) {
				return ShapeHit{};
			}

			const core::Vector3 point = ray.PointAt(distance);
			return ShapeHit{true, distance, (point - shape.Frame.Position).Unit()};
		}

		ShapeHit CylinderRay(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
			const float radius = shape.Extent.X;
			const float halfHeight = shape.Extent.Y;
			const core::Vector3 origin = ToLocalPoint(shape.Frame, ray.Origin);
			const core::Vector3 direction = ToLocalVector(shape.Frame, ray.Direction);

			// A cylinder is a slab crossed with an infinite tube, so the test
			// is the two intervals overlapped. Doing it as two intervals rather
			// than as a barrel test plus two cap tests is what makes the
			// entering feature fall out of the arithmetic instead of needing a
			// second round of comparisons to recover.
			Interval slab;
			bool capEntry = false;
			if (std::abs(direction.Y) < PARALLEL_EPSILON) {
				slab.Empty = std::abs(origin.Y) > halfHeight;
			} else {
				const float inverse = 1.0f / direction.Y;
				const float first = (-halfHeight - origin.Y) * inverse;
				const float second = (halfHeight - origin.Y) * inverse;
				slab.Entry = first < second ? first : second;
				slab.Exit = first < second ? second : first;
			}

			Interval tube;
			const float quadratic = direction.X * direction.X + direction.Z * direction.Z;
			const float linear = 2.0f * (origin.X * direction.X + origin.Z * direction.Z);
			const float constant = origin.X * origin.X + origin.Z * origin.Z - radius * radius;
			if (quadratic < PARALLEL_EPSILON) {
				tube.Empty = constant > 0.0f;
			} else {
				const float discriminant = linear * linear - 4.0f * quadratic * constant;
				if (discriminant < 0.0f) {
					tube.Empty = true;
				} else {
					const float root = std::sqrt(discriminant);
					tube.Entry = (-linear - root) / (2.0f * quadratic);
					tube.Exit = (-linear + root) / (2.0f * quadratic);
				}
			}

			if (slab.Empty || tube.Empty) {
				return ShapeHit{};
			}

			float entry = slab.Entry;
			capEntry = true;
			if (tube.Entry > entry) {
				entry = tube.Entry;
				capEntry = false;
			}
			const float exit = slab.Exit < tube.Exit ? slab.Exit : tube.Exit;

			if (entry > exit || exit < 0.0f || entry > maxDistance) {
				return ShapeHit{};
			}

			core::Vector3 local;
			if (capEntry) {
				local = core::Vector3{0.0f, direction.Y > 0.0f ? -1.0f : 1.0f, 0.0f};
			} else {
				const core::Vector3 at = origin + direction * entry;
				const float across = std::sqrt(at.X * at.X + at.Z * at.Z);
				local =
					across > 0.0f ? core::Vector3{at.X / across, 0.0f, at.Z / across} : core::Vector3::XAxis;
			}

			return ShapeHit{true, entry > 0.0f ? entry : 0.0f, shape.Frame.VectorToWorldSpace(local)};
		}
	}

	ShapeHit IntersectRayShape(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
		switch (shape.Shape) {
		case scene::ShapeKind::Box:
			return BoxRay(shape, ray, maxDistance);
		case scene::ShapeKind::Sphere:
			return SphereRay(shape, ray, maxDistance);
		case scene::ShapeKind::Cylinder:
			return CylinderRay(shape, ray, maxDistance);
		}

		// A shape kind off a wire that this build does not know: no hit, for
		// the same reason `ProjectionRadius` gives it no reach.
		return ShapeHit{};
	}
}
