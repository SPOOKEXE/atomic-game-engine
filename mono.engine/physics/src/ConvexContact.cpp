#include "ContactPairs.hpp"
#include "ConvexQuery.hpp"
#include "FaceManifold.hpp"
#include "ShapeSupport.hpp"

#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

	namespace {
		// **The limit, the triangle-as-a-hull and the reach all moved to
		// `ShapeSupport.hpp` at v0.19**, because the sweep needs the same three
		// and two copies of "how many triangles may one query look at" is two
		// answers to the same question. See `MAXIMUM_MESH_TRIANGLES`,
		// `FillTriangleHull` and `ShapeReach` there.

		// The contact between two shapes neither of which is a mesh.
		//
		// GJK decides whether they touch, EPA gives the axis and the depth, and
		// the face clip every other pair uses turns that into points.
		ContactSolution ConvexPair(const ShapeInstance &first, const ShapeInstance &second) {
			const ConvexPenetration hit = PenetrationBetween(first, second);
			if (!hit.Overlapping) {
				return ContactSolution{};
			}

			// **A zero depth still goes through the manifold clip.** Two shapes
			// resting exactly on each other overlap by nothing and are held apart
			// by the same four points as two shapes overlapping by a millimetre;
			// answering with a single point there would make a hull go from
			// resting to rocking as it settled.
			return ManifoldBetween(first, second, hit.Normal, hit.Depth);
		}

		// A separated box pair is admitted only when every separating direction
		// is parallel to one face normal. Two independent gaps are an edge or
		// corner distance, where the largest SAT gap is not the Euclidean distance.
		// Those ambiguous cases stay discrete instead of manufacturing a witness
		// outside either box.
		SeparatedContact
		SeparatedBoxFacePair(const ShapeInstance &first, const ShapeInstance &second, float maximumDistance) {
			core::Vector3 axes[15];
			size_t count = 0;
			for (size_t axis = 0; axis < 3; axis++) {
				axes[count++] = first.Axis[axis];
				axes[count++] = second.Axis[axis];
			}
			for (size_t left = 0; left < 3; left++) {
				for (size_t right = 0; right < 3; right++) {
					axes[count++] = first.Axis[left].Cross(second.Axis[right]);
				}
			}

			const core::Vector3 offset = second.Frame.Position - first.Frame.Position;
			float gapDistance = 0.0f;
			core::Vector3 normal;
			for (size_t at = 0; at < count; at++) {
				const float lengthSquared = axes[at].MagnitudeSquared();
				if (lengthSquared < CONVEX_EPSILON * CONVEX_EPSILON) {
					continue;
				}
				const core::Vector3 axis = axes[at] * (1.0f / std::sqrt(lengthSquared));
				const float projected = offset.Dot(axis);
				const float gap =
					std::abs(projected) - ProjectionRadius(first, axis) - ProjectionRadius(second, axis);
				if (gap > maximumDistance) {
					return {};
				}
				if (!(gap > CONVEX_EPSILON)) {
					continue;
				}
				const core::Vector3 oriented = projected >= 0.0f ? axis : -axis;
				if (gapDistance > 0.0f && std::abs(oriented.Dot(normal)) < 0.9999f) {
					return {};
				}
				if (gap > gapDistance) {
					gapDistance = gap;
					normal = oriented;
				}
			}
			if (!(gapDistance > 0.0f)) {
				return {};
			}

			const auto faceCentre = [](const ShapeInstance &box, const core::Vector3 &direction) {
				core::Vector3 point = box.Frame.Position;
				const float extent[3] = {box.Extent.X, box.Extent.Y, box.Extent.Z};
				for (size_t axis = 0; axis < 3; axis++) {
					const float along = box.Axis[axis].Dot(direction);
					if (std::abs(along) > CONVEX_EPSILON) {
						point = point + box.Axis[axis] * (along > 0.0f ? extent[axis] : -extent[axis]);
					}
				}
				return point;
			};
			const core::Vector3 firstSupport = faceCentre(first, normal);
			const core::Vector3 secondSupport = faceCentre(second, -normal);
			const core::Vector3 tangent = (secondSupport - firstSupport) - normal * gapDistance;
			return SeparatedContact{
				normal,
				firstSupport + tangent * 0.5f,
				secondSupport - tangent * 0.5f,
				gapDistance,
				0,
				true,
			};
		}

		// The contact between a mesh and something convex.
		//
		// **The deepest contacts across the triangles the shape reaches**, kept
		// to `ContactManifold::MAXIMUM_POINTS`. Deepest rather than first,
		// because the triangles a box rests across contribute one point each and
		// the ones holding it up are the ones it has sunk furthest into - a
		// first-four rule would keep whichever four the mesh happened to store
		// earliest and let the box tip over the rest.
		ContactSolution MeshPair(const ShapeInstance &convex, const ShapeInstance &mesh, bool meshIsSecond) {
			// The convex shape's reach, in the mesh's own space, because the
			// mesh's triangles are in that space and moving one box there is
			// cheaper than moving every triangle out of it.
			const core::AABB world = ShapeReach(convex);
			const core::Vector3 corners[8] = {
				core::Vector3{world.Minimum.X, world.Minimum.Y, world.Minimum.Z},
				core::Vector3{world.Maximum.X, world.Minimum.Y, world.Minimum.Z},
				core::Vector3{world.Minimum.X, world.Maximum.Y, world.Minimum.Z},
				core::Vector3{world.Maximum.X, world.Maximum.Y, world.Minimum.Z},
				core::Vector3{world.Minimum.X, world.Minimum.Y, world.Maximum.Z},
				core::Vector3{world.Maximum.X, world.Minimum.Y, world.Maximum.Z},
				core::Vector3{world.Minimum.X, world.Maximum.Y, world.Maximum.Z},
				core::Vector3{world.Maximum.X, world.Maximum.Y, world.Maximum.Z},
			};

			core::AABB local;
			for (size_t index = 0; index < 8; index++) {
				const core::Vector3 point = ToLocalPoint(mesh.Frame, corners[index]);
				const core::AABB one{point, point};
				local = index == 0 ? one : local.Union(one);
			}

			std::array<uint32_t, MAXIMUM_MESH_TRIANGLES> reached{};
			const size_t count = collision::OverlapTriangles(*mesh.Mesh, local, reached);

			// Every contact found, before the deepest are kept.
			struct Found {
				core::Vector3 Position;
				core::Vector3 Normal;
				float Depth = 0.0f;
				uint32_t Feature = 0;
			};
			std::array<Found, MAXIMUM_MESH_TRIANGLES> found{};
			size_t hits = 0;

			// The triangle being tested, reused across the walk - so a body over
			// sixty triangles allocates its three points once rather than sixty
			// times, which is the allocation rule the rest of this module keeps.
			collision::ConvexHull triangle;

			for (size_t at = 0; at < count; at++) {
				FillTriangleHull(*mesh.Mesh, reached[at], triangle);
				const ShapeInstance placed{
					mesh.Frame, core::Vector3::Zero, scene::ShapeKind::Hull, &triangle, nullptr
				};

				// **Solved in the caller's order, so the normal comes out of the
				// pair the right way round.** A triangle standing in for the mesh
				// has to sit on whichever side the mesh was on.
				const ContactSolution solution =
					meshIsSecond ? ConvexPair(convex, placed) : ConvexPair(placed, convex);
				if (!solution.Touching) {
					continue;
				}

				for (size_t point = 0; point < solution.PointCount && hits < found.size(); point++) {
					found[hits].Position = solution.Positions[point];
					found[hits].Normal = solution.Normal;
					found[hits].Depth = solution.Penetrations[point];

					// **The triangle's index is the feature key**, so the warm
					// start finds last tick's impulse for the same triangle. A
					// mesh's triangle order is fixed by its build, which is what
					// makes that stable.
					found[hits].Feature = ContactFeature(
						static_cast<uint8_t>(reached[at] & 0xFFu),
						static_cast<uint8_t>((reached[at] >> 8) & 0xFFu),
						point
					);
					hits++;
				}
			}

			if (hits == 0) {
				return ContactSolution{};
			}

			// Deepest first, with the feature key breaking a tie so the answer is
			// a function of the mesh rather than of the order the walk took.
			std::sort(
				found.begin(), found.begin() + static_cast<long>(hits), [](const Found &a, const Found &b) {
					if (a.Depth != b.Depth) {
						return a.Depth > b.Depth;
					}
					return a.Feature < b.Feature;
				}
			);

			ContactSolution solution;
			solution.Touching = true;
			solution.Normal = found[0].Normal;
			solution.PointCount =
				static_cast<uint8_t>(std::min<size_t>(hits, ContactManifold::MAXIMUM_POINTS));
			for (size_t point = 0; point < solution.PointCount; point++) {
				solution.Positions[point] = found[point].Position;
				solution.Penetrations[point] = found[point].Depth;
				solution.Features[point] = found[point].Feature;
			}
			return solution;
		}
	}

	ContactSolution ConvexContact(const ShapeInstance &first, const ShapeInstance &second) {
		const bool firstIsMesh = first.Shape == scene::ShapeKind::Mesh;
		const bool secondIsMesh = second.Shape == scene::ShapeKind::Mesh;

		if (firstIsMesh && secondIsMesh) {
			// **Two soups never touch, and that is the design.** Both are
			// surfaces with no inside, so there is no overlap to resolve and no
			// direction to push either of them - and a mesh collider is static
			// geometry, so a pair of them is two pieces of level that were
			// authored where they are. Reported as no contact rather than
			// refused, because a level author overlapping two terrain chunks has
			// not made a mistake.
			return ContactSolution{};
		}

		if (secondIsMesh) {
			return MeshPair(first, second, true);
		}
		if (firstIsMesh) {
			return MeshPair(second, first, false);
		}
		return ConvexPair(first, second);
	}

	SeparatedContact
	SeparatedBetween(const ShapeInstance &first, const ShapeInstance &second, float maximumDistance) {
		SeparatedContact answer;
		if (!(maximumDistance > 0.0f)) {
			return answer;
		}

		const bool firstIsMesh = first.Shape == scene::ShapeKind::Mesh;
		const bool secondIsMesh = second.Shape == scene::ShapeKind::Mesh;
		if (firstIsMesh || secondIsMesh) {
			return answer;
		}
		if (first.Shape == scene::ShapeKind::Box && second.Shape == scene::ShapeKind::Box) {
			return SeparatedBoxFacePair(first, second, maximumDistance);
		}

		const ConvexSeparation gap = ClosestPoints(first, second);
		if (gap.Overlapping || !(gap.Distance > CONVEX_EPSILON) || gap.Distance > maximumDistance) {
			return answer;
		}

		answer.Normal = (gap.OnSecond - gap.OnFirst).Unit();
		answer.OnFirst = gap.OnFirst;
		answer.OnSecond = gap.OnSecond;
		answer.Distance = gap.Distance;
		answer.Found = answer.Normal.MagnitudeSquared() > 0.0f;
		return answer;
	}
}
