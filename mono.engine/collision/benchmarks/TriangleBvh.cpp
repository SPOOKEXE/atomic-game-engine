#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.collision.bench.triangle-bvh")

using engine::collision::BuildTriangleMesh;
using engine::collision::OverlapTriangles;
using engine::collision::TriangleMesh;
using engine::core::AABB;
using engine::core::Vector3;
using engine::testing::Consume;

namespace {
	TriangleMesh Ground(uint32_t side) {
		std::vector<Vector3> vertices;
		std::vector<uint32_t> indices;
		for (uint32_t z = 0; z <= side; z++) {
			for (uint32_t x = 0; x <= side; x++) {
				vertices.push_back(Vector3{static_cast<float>(x), 0.0f, static_cast<float>(z)});
			}
		}
		for (uint32_t z = 0; z < side; z++) {
			for (uint32_t x = 0; x < side; x++) {
				const uint32_t corner = z * (side + 1) + x;
				indices.insert(
					indices.end(),
					{corner, corner + side + 1, corner + 1, corner + 1, corner + side + 1, corner + side + 2}
				);
			}
		}
		return BuildTriangleMesh(vertices, indices);
	}

	const TriangleMesh &Terrain() {
		static const TriangleMesh mesh = Ground(256);
		return mesh;
	}

	const AABB QUERY{Vector3{127.6f, -1.0f, 127.6f}, Vector3{128.4f, 1.0f, 128.4f}};
}

BENCH("Bounds scan · 131072 triangles", 100) {
	const TriangleMesh &mesh = Terrain();
	for (int pass = 0; pass < 100; pass++) {
		size_t found = 0;
		for (const AABB &bound : mesh.TriangleBounds) {
			found += bound.Overlaps(QUERY) ? 1 : 0;
		}
		Consume(found);
	}
}

BENCH("Triangle BVH · 131072 triangles", 100) {
	const TriangleMesh &mesh = Terrain();
	for (int pass = 0; pass < 100; pass++) {
		uint32_t found[32] = {};
		Consume(OverlapTriangles(mesh, QUERY, found));
	}
}
