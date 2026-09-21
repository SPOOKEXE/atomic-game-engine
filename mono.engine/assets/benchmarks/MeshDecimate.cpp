// Automatic LOD build latency for the topology used by PbrShaderMaterialDemo.

#include <engine/assets/MeshDecimate.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cmath>

TEST_SUITE_ID("engine.assets.bench.mesh-decimate")

namespace {
	engine::assets::MeshData ReliefSphere() {
		using engine::assets::MeshData;
		using engine::assets::MeshVertex;
		constexpr size_t RINGS = 84;
		constexpr size_t SEGMENTS = 112;
		constexpr float PI = 3.14159265358979323846f;
		MeshData mesh;
		mesh.Vertices.reserve((RINGS + 1) * (SEGMENTS + 1));
		mesh.Indices.reserve(RINGS * SEGMENTS * 6);
		for (size_t ring = 0; ring <= RINGS; ring++) {
			const float v = static_cast<float>(ring) / RINGS;
			const float theta = v * PI;
			for (size_t segment = 0; segment <= SEGMENTS; segment++) {
				const float u = static_cast<float>(segment) / SEGMENTS;
				const float phi = u * PI * 2.0f;
				const float x = std::sin(theta) * std::cos(phi);
				const float y = std::cos(theta);
				const float z = std::sin(theta) * std::sin(phi);
				const float radius = 2.75f + std::sin(u * 41.0f) * std::cos(v * 37.0f) * 0.12f;
				MeshVertex vertex{};
				vertex.Position[0] = x * radius;
				vertex.Position[1] = y * radius;
				vertex.Position[2] = z * radius;
				vertex.Normal[0] = x;
				vertex.Normal[1] = y;
				vertex.Normal[2] = z;
				vertex.TexCoord[0] = u;
				vertex.TexCoord[1] = v;
				mesh.Vertices.push_back(vertex);
			}
		}
		for (size_t ring = 0; ring < RINGS; ring++) {
			for (size_t segment = 0; segment < SEGMENTS; segment++) {
				const uint32_t a = static_cast<uint32_t>(ring * (SEGMENTS + 1) + segment);
				const uint32_t b = a + 1;
				const uint32_t c = a + SEGMENTS + 1;
				mesh.Indices.insert(mesh.Indices.end(), {a, b, c, b, c + 1, c});
			}
		}
		mesh.ComputeBounds();
		return mesh;
	}
}

BENCH("PBR relief sphere automatic LOD ladder", 1) {
	static const engine::assets::MeshData source = ReliefSphere();
	std::array<engine::assets::MeshData, 3> ladder;
	const std::array ratios{0.7f, 0.4f, 0.1f};
	engine::testing::Consume(engine::assets::BuildMeshLodLadder(source, ratios, ladder));
}
