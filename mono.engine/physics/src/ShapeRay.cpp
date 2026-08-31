#include "ShapeRay.hpp"

#include "ShapeSupport.hpp"

#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>

#include <algorithm>
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
			// not it is here, which is why the rotated-box raycast deserves
			// its own test.
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

		ShapeHit CapsuleRay(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
			const float radius = shape.Extent.X;
			const float halfSegment = shape.Extent.Y;
			const core::Vector3 origin = ToLocalPoint(shape.Frame, ray.Origin);
			const core::Vector3 direction = ToLocalVector(shape.Frame, ray.Direction);
			const float closestY = std::clamp(origin.Y, -halfSegment, halfSegment);
			const core::Vector3 closest{0.0f, closestY, 0.0f};
			const core::Vector3 fromSegment = origin - closest;

			if (fromSegment.MagnitudeSquared() <= radius * radius) {
				const core::Vector3 localNormal =
					fromSegment.MagnitudeSquared() > 0.0f ? fromSegment.Unit() : -direction;
				return ShapeHit{true, 0.0f, shape.Frame.VectorToWorldSpace(localNormal)};
			}

			ShapeHit nearest;
			nearest.Distance = maxDistance;

			// The straight barrel has no caps. Its roots are accepted only while
			// the hit lies between the two hemisphere centres.
			const float quadratic = direction.X * direction.X + direction.Z * direction.Z;
			if (quadratic >= PARALLEL_EPSILON) {
				const float linear = 2.0f * (origin.X * direction.X + origin.Z * direction.Z);
				const float constant = origin.X * origin.X + origin.Z * origin.Z - radius * radius;
				const float discriminant = linear * linear - 4.0f * quadratic * constant;
				if (discriminant >= 0.0f) {
					const float root = std::sqrt(discriminant);
					const float roots[2] = {
						(-linear - root) / (2.0f * quadratic),
						(-linear + root) / (2.0f * quadratic),
					};
					for (const float distance : roots) {
						const float y = origin.Y + direction.Y * distance;
						if (distance < 0.0f || distance > nearest.Distance || std::abs(y) > halfSegment) {
							continue;
						}
						const core::Vector3 point = origin + direction * distance;
						nearest = ShapeHit{
							true,
							distance,
							shape.Frame.VectorToWorldSpace(core::Vector3{point.X, 0.0f, point.Z}.Unit()),
						};
					}
				}
			}

			// Each sphere contributes only its outward hemisphere. The other half
			// lies inside the barrel and is not part of the capsule surface.
			for (int side = -1; side <= 1; side += 2) {
				const core::Vector3 centre{0.0f, halfSegment * static_cast<float>(side), 0.0f};
				const core::Vector3 offset = origin - centre;
				const float along = offset.Dot(direction);
				const float gap = offset.MagnitudeSquared() - radius * radius;
				const float discriminant = along * along - gap;
				if (discriminant < 0.0f) {
					continue;
				}

				const float distance = -along - std::sqrt(discriminant);
				if (distance < 0.0f || distance > nearest.Distance) {
					continue;
				}
				const core::Vector3 point = origin + direction * distance;
				if ((side > 0 && point.Y < halfSegment) || (side < 0 && point.Y > -halfSegment)) {
					continue;
				}
				nearest = ShapeHit{
					true,
					distance,
					shape.Frame.VectorToWorldSpace((point - centre).Unit()),
				};
			}

			return nearest.Touched ? nearest : ShapeHit{};
		}
	}

	namespace {
		// A ray against a convex hull, in the hull's own space.
		//
		// **Clipped against every face plane, which is the whole test.** A convex
		// polyhedron is the intersection of its half-spaces, so the segment
		// inside it is the interval that survives clipping against all of them -
		// the same slab method `RayBox` uses for a box, with the hull's own
		// planes instead of three axis pairs. Exact, and linear in the face count
		// rather than in the point count.
		//
		// **A hull with no faces is answered by its bound.** Flat, straight and
		// single-point hulls are ordinary output from
		// `collision::BuildConvexHull` - see its header - and none of them
		// encloses a volume for a ray to enter. The bound is the conservative
		// answer, which is the direction a raycast may be wrong in: an extra hit
		// on a shape the caller can see, rather than a missing one.
		ShapeHit HullRay(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
			const core::Vector3 origin = ToLocalPoint(shape.Frame, ray.Origin);
			const core::Vector3 direction = ToLocalVector(shape.Frame, ray.Direction);

			if (!shape.Hull->Solid()) {
				ShapeInstance bound{
					shape.Frame,
					core::Vector3{
						std::max(
							std::abs(shape.Hull->Bounds.Minimum.X), std::abs(shape.Hull->Bounds.Maximum.X)
						),
						std::max(
							std::abs(shape.Hull->Bounds.Minimum.Y), std::abs(shape.Hull->Bounds.Maximum.Y)
						),
						std::max(
							std::abs(shape.Hull->Bounds.Minimum.Z), std::abs(shape.Hull->Bounds.Maximum.Z)
						),
					},
					scene::ShapeKind::Box,
				};
				return BoxRay(bound, ray, maxDistance);
			}

			float entry = 0.0f;
			float exit = maxDistance;
			core::Vector3 entered;
			bool inside = true;

			for (const collision::HullFace &face : shape.Hull->Faces) {
				const float towards = face.Normal.Dot(direction);
				const float above = face.Normal.Dot(origin) - face.Offset;

				if (std::abs(towards) < 1e-8f) {
					// Parallel to this plane. Outside it means outside the hull
					// for the whole ray; inside it constrains nothing.
					if (above > 0.0f) {
						return ShapeHit{};
					}
					continue;
				}

				const float crossing = -above / towards;
				if (towards < 0.0f) {
					// Entering through this face.
					if (crossing > entry) {
						entry = crossing;
						entered = face.Normal;
						inside = false;
					}
				} else if (crossing < exit) {
					exit = crossing;
				}

				if (entry > exit) {
					return ShapeHit{};
				}
			}

			if (entry > maxDistance) {
				return ShapeHit{};
			}

			// An origin inside reports zero and the direction it came from, which
			// is this file's stated convention for every shape.
			if (inside) {
				return ShapeHit{true, 0.0f, ray.Direction * -1.0f};
			}
			return ShapeHit{true, entry, shape.Frame.VectorToWorldSpace(entered)};
		}

		// A ray against a triangle soup.
		//
		// **The nearest triangle, by Moller-Trumbore against each one the ray's
		// own bound reaches.** There is no index here - `collision/AGENTS.md`
		// explains why the module has none - so the candidates come from the
		// mesh's per-triangle bounds against the segment's box, which is the same
		// rejection a scan would do and is what makes it affordable.
		//
		// **The normal is turned to face the ray**, because a soup has no inside:
		// a triangle's winding says which way it was authored, and a caller
		// raycasting terrain wants the surface it hit rather than a normal
		// pointing away from it because the level was modelled the other way up.
		ShapeHit MeshRay(const ShapeInstance &shape, const core::Ray &ray, float maxDistance) {
			const core::Vector3 origin = ToLocalPoint(shape.Frame, ray.Origin);
			const core::Vector3 direction = ToLocalVector(shape.Frame, ray.Direction);
			const core::Vector3 finish = origin + direction * maxDistance;

			const core::AABB segment = core::AABB{origin, origin}.Union(core::AABB{finish, finish});

			ShapeHit nearest;
			nearest.Distance = maxDistance;

			for (size_t triangle = 0; triangle < shape.Mesh->TriangleCount(); triangle++) {
				if (!shape.Mesh->TriangleBounds[triangle].Overlaps(segment)) {
					continue;
				}

				const collision::Triangle corners = shape.Mesh->TriangleAt(triangle);
				const core::Vector3 edgeA = corners.B - corners.A;
				const core::Vector3 edgeB = corners.C - corners.A;
				const core::Vector3 across = direction.Cross(edgeB);
				const float determinant = edgeA.Dot(across);

				// Parallel to the triangle's plane. Not a hit either way: a ray
				// travelling along a surface has no entry point.
				if (std::abs(determinant) < 1e-9f) {
					continue;
				}

				const float inverse = 1.0f / determinant;
				const core::Vector3 toCorner = origin - corners.A;
				const float u = toCorner.Dot(across) * inverse;
				if (u < 0.0f || u > 1.0f) {
					continue;
				}

				const core::Vector3 sideways = toCorner.Cross(edgeA);
				const float v = direction.Dot(sideways) * inverse;
				if (v < 0.0f || u + v > 1.0f) {
					continue;
				}

				const float distance = edgeB.Dot(sideways) * inverse;
				if (distance < 0.0f || distance >= nearest.Distance) {
					continue;
				}

				core::Vector3 normal = edgeA.Cross(edgeB).Unit();
				if (normal.Dot(direction) > 0.0f) {
					normal = normal * -1.0f;
				}

				nearest.Touched = true;
				nearest.Distance = distance;
				nearest.Normal = shape.Frame.VectorToWorldSpace(normal);
			}

			if (!nearest.Touched) {
				return ShapeHit{};
			}
			return nearest;
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
		case scene::ShapeKind::Capsule:
			return CapsuleRay(shape, ray, maxDistance);

		case scene::ShapeKind::Hull:
			return HullRay(shape, ray, maxDistance);

		case scene::ShapeKind::Mesh:
			return MeshRay(shape, ray, maxDistance);
		}

		// A shape kind off a wire that this build does not know: no hit, for
		// the same reason `ProjectionRadius` gives it no reach.
		return ShapeHit{};
	}
}
