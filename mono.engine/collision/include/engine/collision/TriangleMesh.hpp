#pragma once

// A triangle soup, for the collider that has to be the mesh rather than a
// convex stand-in for it.
//
// **A soup and not a solid.** There is no inside here: a triangle mesh collides
// as a surface, so a body resting on terrain is held up by the triangles it
// touches and a body that has been teleported *through* the surface is not
// pushed back out - nothing here can tell which side it should have been on.
// That is the standing difference between this and `ConvexHull`, and it is why a
// mesh collider is for static geometry. A dynamic body wants a hull.
//
// **One immutable local-space hierarchy per mesh.** Every placed instance shares
// it, so build cost and storage follow geometry rather than collider count. The
// hierarchy lives here because it indexes triangle data only and has no world,
// layer, or physics policy.
//
// @tier L5 · shared
// @since v0.17

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::collision {

	// One triangle, resolved.
	//
	// **Handed back by value rather than as three indices**, because every
	// caller immediately wants the points and resolving them twice - once to
	// decide and once to use - is the indirection this exists to spend once.
	//
	// @since v0.17
	struct Triangle {
		// The three corners, in the winding the mesh stored them. Winding is
		// what decides the face normal, so a caller that reorders these has
		// turned the triangle inside out.
		//@{
		core::Vector3 A;
		core::Vector3 B;
		core::Vector3 C;
		//@}
	};

	struct TriangleBvhNode {
		core::AABB Bounds;
		uint32_t First = 0;
		uint32_t Count = 0;
		uint32_t Left = 0;
		uint32_t Right = 0;

		bool Leaf() const {
			return Count != 0;
		}
	};

	// A triangle mesh in its own object space.
	//
	// @since v0.17
	struct TriangleMesh {
		// The vertices, referenced by `Indices`.
		std::vector<core::Vector3> Vertices;

		// Triangle list. Always a multiple of three.
		std::vector<uint32_t> Indices;

		// Each triangle's own bound, parallel to the triangle list.
		//
		// **Stored rather than derived per query.** A reached leaf rejects its
		// triangles against six floats already in order; deriving each bound there
		// would add three scattered vertex lookups to the narrowest part of the
		// query.
		std::vector<core::AABB> TriangleBounds;

		// Median-split hierarchy and its leaf triangle order. Leaves hold at most
		// four triangles, while public triangle ids remain the original ids.
		std::vector<TriangleBvhNode> Hierarchy;
		std::vector<uint32_t> HierarchyTriangles;

		// The object-space bound of the whole mesh, derived by `BuildTriangleMesh`.
		core::AABB Bounds;

		// How many triangles there are.
		size_t TriangleCount() const {
			return Indices.size() / 3;
		}

		// One triangle's three corners.
		//
		// @param triangle An index below `TriangleCount()`.
		Triangle TriangleAt(size_t triangle) const {
			return Triangle{
				Vertices[Indices[triangle * 3]],
				Vertices[Indices[triangle * 3 + 1]],
				Vertices[Indices[triangle * 3 + 2]],
			};
		}
	};

	// Builds a mesh from vertices and a triangle list.
	//
	// **Every triangle is checked and the bad ones are dropped, because the
	// input is a file.** An index past the end of the vertex array, a triangle
	// with two corners the same, and a coordinate that is not finite are all
	// things a crafted or a merely broken mesh contains, and each of them
	// reaches a narrow phase as an answer rather than as a failure: a degenerate
	// triangle has no normal, so a contact against one has a direction of NaN,
	// and one NaN in a velocity is a body that leaves the world and never comes
	// back.
	//
	// **A count that is not a multiple of three loses its tail** rather than
	// being refused, which is the same choice `assets::MeshData` makes: a file
	// with one stray index is a file with one stray index, and refusing the
	// whole model over it is the less useful answer.
	//
	// @param vertices The corners.
	// @param indices  Triangle list, three indices per triangle.
	// @return The mesh, with the bad triangles gone.
	TriangleMesh
	BuildTriangleMesh(std::span<const core::Vector3> vertices, std::span<const uint32_t> indices);

	// Every triangle whose own bound overlaps `box`, by index.
	//
	// Traverses the mesh's immutable local hierarchy. Results are sorted by
	// original triangle id, independent of hierarchy shape.
	//
	// @param mesh The mesh, in the same space as `box`.
	// @param box  The query volume, in the mesh's own space.
	// @param out  Where the indices are written, in ascending order.
	// @return How many were written, which is capped at `out.size()`.
	size_t OverlapTriangles(const TriangleMesh &mesh, const core::AABB &box, std::span<uint32_t> out);

	// The closest point of a triangle to a point.
	//
	// The standard barycentric region test - Ericson, *Real-Time Collision
	// Detection*, section 5.1.5. Exact for a degenerate triangle too, which is
	// why `BuildTriangleMesh` dropping those is a speed decision rather than a
	// correctness one.
	//
	// @param triangle The triangle.
	// @param point    The point, in the same space.
	// @return The closest point on or inside the triangle.
	core::Vector3 ClosestPointOnTriangle(const Triangle &triangle, const core::Vector3 &point);
}
