#include <engine/collision/TriangleMesh.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.collision.trianglemesh")
TEST_DEPENDS("engine.core.types.vector3")
TEST_DEPENDS("engine.core.types.aabb")

using Catch::Approx;
using engine::collision::BuildTriangleMesh;
using engine::collision::ClosestPointOnTriangle;
using engine::collision::OverlapTriangles;
using engine::collision::Triangle;
using engine::collision::TriangleMesh;
using engine::core::AABB;
using engine::core::Vector3;

namespace {
	// A flat grid of quads on the XZ plane, `side` quads on an edge, each one
	// metre across, with its corner at the origin.
	TriangleMesh Ground(uint32_t side) {
		std::vector<Vector3> vertices;
		for (uint32_t z = 0; z <= side; z++) {
			for (uint32_t x = 0; x <= side; x++) {
				vertices.push_back(Vector3{static_cast<float>(x), 0.0f, static_cast<float>(z)});
			}
		}

		std::vector<uint32_t> indices;
		for (uint32_t z = 0; z < side; z++) {
			for (uint32_t x = 0; x < side; x++) {
				const uint32_t corner = z * (side + 1) + x;
				indices.push_back(corner);
				indices.push_back(corner + side + 1);
				indices.push_back(corner + 1);
				indices.push_back(corner + 1);
				indices.push_back(corner + side + 1);
				indices.push_back(corner + side + 2);
			}
		}

		return BuildTriangleMesh(vertices, indices);
	}

	AABB Around(const Vector3 &centre, float half) {
		return AABB{
			Vector3{centre.X - half, centre.Y - half, centre.Z - half},
			Vector3{centre.X + half, centre.Y + half, centre.Z + half},
		};
	}
}

TEST_CASE("a mesh keeps its triangles and derives its bound", "[trianglemesh]") {
	const TriangleMesh mesh = Ground(4);
	CHECK(mesh.TriangleCount() == 32);
	CHECK(mesh.TriangleBounds.size() == 32);
	CHECK(mesh.Bounds.Minimum == Vector3::Zero);
	CHECK(mesh.Bounds.Maximum == Vector3{4.0f, 0.0f, 4.0f});
}

TEST_CASE("an index past the end drops its triangle", "[trianglemesh]") {
	// The one failure here that is not merely a bad answer - it is a read of
	// somebody else's memory, from a file.
	const std::vector<Vector3> vertices{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
	};
	const std::vector<uint32_t> indices{0, 1, 2, 0, 1, 99};

	const TriangleMesh mesh = BuildTriangleMesh(vertices, indices);
	CHECK(mesh.TriangleCount() == 1);
}

TEST_CASE("a degenerate triangle drops out", "[trianglemesh]") {
	// A contact against a triangle with no area has a normal of NaN, and one
	// NaN in a velocity is a body that leaves the world and never comes back.
	//
	// **Three collinear corners, not two equal ones**, because that is the case
	// a check for repeated indices misses.
	const std::vector<Vector3> vertices{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{2.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
	};
	const std::vector<uint32_t> indices{0, 1, 2, 0, 1, 3};

	const TriangleMesh mesh = BuildTriangleMesh(vertices, indices);
	CHECK(mesh.TriangleCount() == 1);
}

TEST_CASE("a vertex that is not a number drops its triangle", "[trianglemesh]") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const std::vector<Vector3> vertices{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
		Vector3{nan, nan, nan},
	};
	const std::vector<uint32_t> indices{0, 1, 2, 0, 1, 3};

	const TriangleMesh mesh = BuildTriangleMesh(vertices, indices);
	CHECK(mesh.TriangleCount() == 1);

	// **And the bound is the union of the triangles that survived**, not of the
	// vertices - a vertex no live triangle names is not part of this collider,
	// and a NaN in the bound is a shape the broad phase cannot reason about.
	CHECK(mesh.Bounds.Maximum.X == Approx(1.0f));
}

TEST_CASE("a stray index loses only the tail", "[trianglemesh]") {
	// A file with one stray index is a file with one stray index; refusing the
	// whole model over it is the less useful answer, and is not what
	// `assets::MeshData` does either.
	const std::vector<Vector3> vertices{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
	};
	const std::vector<uint32_t> indices{0, 1, 2, 0, 1};

	CHECK(BuildTriangleMesh(vertices, indices).TriangleCount() == 1);
}

TEST_CASE("an overlap reports the triangles under the box and no others", "[trianglemesh]") {
	const TriangleMesh mesh = Ground(8);

	uint32_t found[64] = {};
	const size_t written = OverlapTriangles(mesh, Around(Vector3{0.5f, 0.0f, 0.5f}, 0.4f), found);

	// The two triangles of the first quad, and nothing from the quads beside it.
	REQUIRE(written == 2);
	CHECK(found[0] == 0);
	CHECK(found[1] == 1);

	// Ascending, which is what lets a caller merge the answer against a list
	// ordered the same way rather than search it.
	CHECK(found[0] < found[1]);
}

TEST_CASE("an overlap nowhere near the mesh costs one box test", "[trianglemesh]") {
	// Not observable as a count, but the answer is: the whole-mesh bound is
	// tested before any triangle is.
	const TriangleMesh mesh = Ground(8);

	uint32_t found[8] = {};
	CHECK(OverlapTriangles(mesh, Around(Vector3{500.0f, 500.0f, 500.0f}, 1.0f), found) == 0);
	CHECK(OverlapTriangles(mesh, Around(Vector3{4.0f, 40.0f, 4.0f}, 1.0f), found) == 0);
}

TEST_CASE("an overlap stops at the span it was given", "[trianglemesh]") {
	// The output span is the caller's and is often on the stack, so overrunning
	// it is the one outcome that is not a wrong answer.
	const TriangleMesh mesh = Ground(8);

	uint32_t found[3] = {};
	CHECK(OverlapTriangles(mesh, mesh.Bounds, found) == 3);
}

TEST_CASE("an empty mesh answers nothing rather than reading past its arrays", "[trianglemesh]") {
	const TriangleMesh mesh = BuildTriangleMesh({}, {});
	CHECK(mesh.TriangleCount() == 0);

	uint32_t found[4] = {};
	CHECK(OverlapTriangles(mesh, Around(Vector3::Zero, 10.0f), found) == 0);
}

TEST_CASE("the closest point covers every barycentric region", "[trianglemesh]") {
	// Seven regions - three vertices, three edges and the face - and each one is
	// a different branch. A version that got the edge regions wrong still passes
	// a test that only probes above the middle.
	const Triangle triangle{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{2.0f, 0.0f, 0.0f},
		Vector3{0.0f, 0.0f, 2.0f},
	};

	// The face.
	CHECK(ClosestPointOnTriangle(triangle, Vector3{0.5f, 5.0f, 0.5f}) == Vector3{0.5f, 0.0f, 0.5f});

	// Each vertex, approached from outside its own corner.
	CHECK(ClosestPointOnTriangle(triangle, Vector3{-1.0f, 0.0f, -1.0f}) == triangle.A);
	CHECK(ClosestPointOnTriangle(triangle, Vector3{5.0f, 0.0f, -1.0f}) == triangle.B);
	CHECK(ClosestPointOnTriangle(triangle, Vector3{-1.0f, 0.0f, 5.0f}) == triangle.C);

	// Each edge.
	CHECK(ClosestPointOnTriangle(triangle, Vector3{1.0f, 0.0f, -3.0f}) == Vector3{1.0f, 0.0f, 0.0f});
	CHECK(ClosestPointOnTriangle(triangle, Vector3{-3.0f, 0.0f, 1.0f}) == Vector3{0.0f, 0.0f, 1.0f});

	const Vector3 acrossHypotenuse = ClosestPointOnTriangle(triangle, Vector3{3.0f, 0.0f, 3.0f});
	CHECK(acrossHypotenuse.X == Approx(1.0f));
	CHECK(acrossHypotenuse.Z == Approx(1.0f));
}

TEST_CASE("the closest point on a triangle with no area is a corner", "[trianglemesh]") {
	// `BuildTriangleMesh` drops these, but a caller passing a `Triangle`
	// directly has not been through it - and the face branch divides by the
	// barycentric sum, which is exactly zero here.
	const Triangle sliver{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{2.0f, 0.0f, 0.0f},
	};

	const Vector3 closest = ClosestPointOnTriangle(sliver, Vector3{0.5f, 1.0f, 0.0f});
	CHECK(closest.Y == Approx(0.0f));
	CHECK(closest.Z == Approx(0.0f));
}
