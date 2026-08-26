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
}
