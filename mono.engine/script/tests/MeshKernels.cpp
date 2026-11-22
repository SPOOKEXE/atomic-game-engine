// The `MeshKernels` arithmetic, called directly.
//
// Each kernel gets one worked case and its refusal. The digest case pins the
// output bytes of every kernel over a fixed mesh: those constants are the
// cross-platform claim, so a platform or compiler that rounds differently
// fails here by name.

#include <engine/script/MeshKernels.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

TEST_SUITE_ID("engine.script.meshkernels")
TEST_TIER("fast")

namespace mk = engine::script::mesh_kernels;

namespace {
	template <typename T> mk::Bytes Span(std::vector<T> &values) {
		return {reinterpret_cast<std::byte *>(values.data()), values.size() * sizeof(T)};
	}

	uint64_t Fnv(const std::vector<float> &values) {
		uint64_t hash = 1469598103934665603ull;
		const auto *bytes = reinterpret_cast<const unsigned char *>(values.data());
		for (size_t i = 0; i < values.size() * 4; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
		return hash;
	}

	// A `side` x `side` grid of vertices with a fixed bump, two triangles per
	// cell, and its CSR 4-neighbourhood.
	struct Grid {
		uint32_t Count = 0;
		std::vector<float> Positions;
		std::vector<uint32_t> Triangles, Offsets, Neighbours;
	};

	Grid MakeGrid(uint32_t side) {
		Grid grid;
		grid.Count = side * side;
		for (uint32_t y = 0; y < side; ++y) {
			for (uint32_t x = 0; x < side; ++x) {
				const float fx = float(x) * 0.01f, fy = float(y) * 0.01f;
				grid.Positions.insert(grid.Positions.end(), {fx, fy, float((x * 7 + y * 13) % 17) * 0.001f});
			}
		}
		for (uint32_t y = 0; y + 1 < side; ++y) {
			for (uint32_t x = 0; x + 1 < side; ++x) {
				const uint32_t a = y * side + x, b = a + 1, c = a + side, d = c + 1;
				grid.Triangles.insert(grid.Triangles.end(), {a, b, c, b, d, c});
			}
		}
		grid.Offsets.push_back(0);
		for (uint32_t y = 0; y < side; ++y) {
			for (uint32_t x = 0; x < side; ++x) {
				const uint32_t v = y * side + x;
				if (x > 0) grid.Neighbours.push_back(v - 1);
				if (x + 1 < side) grid.Neighbours.push_back(v + 1);
				if (y > 0) grid.Neighbours.push_back(v - side);
				if (y + 1 < side) grid.Neighbours.push_back(v + side);
				grid.Offsets.push_back(uint32_t(grid.Neighbours.size()));
			}
		}
		return grid;
	}
}

TEST_CASE("Axpy adds a scaled stream and refuses a short one untouched", "[script][meshkernels]") {
	std::vector<float> out = {1, 2, 3}, x = {10, 20, 30};
	CHECK(mk::Axpy(Span(out), 0.5f, Span(x), 3));
	CHECK(out == std::vector<float>{6, 12, 18});
	CHECK_FALSE(mk::Axpy(Span(out), 1.0f, Span(x), 4));
	CHECK(out == std::vector<float>{6, 12, 18});
}

TEST_CASE("WeightedAxpy weights each vertex of a strided stream", "[script][meshkernels]") {
	std::vector<float> out = {0, 0, 0, 0, 0, 0}, weights = {1, 0.5f}, x = {1, 2, 3, 4, 4, 4};
	CHECK(mk::WeightedAxpy(Span(out), Span(weights), Span(x), 2, 3));
	CHECK(out == std::vector<float>{1, 2, 3, 2, 2, 2});
	CHECK_FALSE(mk::WeightedAxpy(Span(out), Span(weights), Span(x), 2, 0));
}

TEST_CASE("Transform blends up to four matrices and refuses a missing one", "[script][meshkernels]") {
	// Matrix 0 is identity, matrix 1 translates by (10, 0, 0).
	std::vector<float> matrices = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0};
	std::vector<float> input = {1, 2, 3}, out(3), weights = {0.5f, 0.5f, 0, 0};
	std::vector<uint16_t> indices = {0, 1, 0, 0};
	CHECK(mk::Transform(Span(out), Span(input), Span(matrices), Span(indices), Span(weights), 1));
	CHECK(out == std::vector<float>{6, 2, 3});

	// A zero weight may name anything; a nonzero one may not.
	indices = {0, 1, 9, 0};
	CHECK(mk::Transform(Span(out), Span(input), Span(matrices), Span(indices), Span(weights), 1));
	weights = {0.5f, 0.25f, 0.25f, 0};
	out = {7, 7, 7};
	CHECK_FALSE(mk::Transform(Span(out), Span(input), Span(matrices), Span(indices), Span(weights), 1));
	CHECK(out == std::vector<float>{7, 7, 7});
}

TEST_CASE("Normals face the winding and a weld shares one across a seam", "[script][meshkernels]") {
	// Two triangles in z = 0, the second using a duplicate of vertex 1 (vertex 3)
	// and tilted up so the two disagree until welded.
	std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1};
	std::vector<uint32_t> triangles = {0, 1, 2, 3, 4, 2};
	std::vector<float> out(15);
	CHECK(mk::Normals(Span(out), Span(positions), Span(triangles), std::nullopt, 5));
	CHECK(out[0] == 0.0f);
	CHECK(out[2] == 1.0f);
	CHECK(out[3 * 3 + 2] != out[1 * 3 + 2]);

	std::vector<uint32_t> weld = {0, 1, 2, 1, 4};
	CHECK(mk::Normals(Span(out), Span(positions), Span(triangles), Span(weld), 5));
	CHECK(std::memcmp(&out[3], &out[9], 12) == 0);

	triangles.push_back(0);
	CHECK_FALSE(mk::Normals(Span(out), Span(positions), Span(triangles), std::nullopt, 5));
}

TEST_CASE("Laplacian relaxes toward neighbours and a zero mask pins", "[script][meshkernels]") {
	// A three-vertex line with the middle one lifted.
	std::vector<float> positions = {0, 0, 0, 1, 1, 0, 2, 0, 0}, out(9);
	std::vector<uint32_t> offsets = {0, 1, 3, 4}, neighbours = {1, 0, 2, 1};
	std::vector<float> mask = {0, 1, 0};
	CHECK(mk::Laplacian(Span(out), Span(positions), Span(offsets), Span(neighbours), Span(mask), 1, 0.5f));
	CHECK(out == std::vector<float>{0, 0, 0, 1, 0.5f, 0, 2, 0, 0});

	// In place, and the iteration cap.
	CHECK(mk::Laplacian(Span(positions), Span(positions), Span(offsets), Span(neighbours), Span(mask), 2, 1.0f));
	CHECK(positions[4] == 0.0f);
	CHECK_FALSE(mk::Laplacian(
		Span(out), Span(positions), Span(offsets), Span(neighbours), std::nullopt, mk::MAX_LAPLACIAN_ITERATIONS + 1,
		1.0f
	));
	neighbours[0] = 3;
	CHECK_FALSE(mk::Laplacian(Span(out), Span(positions), Span(offsets), Span(neighbours), std::nullopt, 1, 1.0f));
}

TEST_CASE("Gather and Scatter remap elements and are inverses", "[script][meshkernels]") {
	std::vector<float> canonical = {1, 1, 2, 2, 3, 3}, display(8);
	std::vector<uint32_t> indices = {2, 0, 0, 1};
	CHECK(mk::Gather(Span(display), Span(canonical), Span(indices), 2));
	CHECK(display == std::vector<float>{3, 3, 1, 1, 1, 1, 2, 2});

	std::vector<float> back(6);
	CHECK(mk::Scatter(Span(back), Span(display), Span(indices), 2));
	CHECK(back == canonical);

	indices[0] = 3;
	CHECK_FALSE(mk::Gather(Span(display), Span(canonical), Span(indices), 2));
	CHECK_FALSE(mk::Gather(Span(display), Span(canonical), Span(indices), 0));
}

TEST_CASE("every kernel writes the same bytes on every platform", "[script][meshkernels]") {
	const Grid grid = MakeGrid(160);
	std::vector<float> positions = grid.Positions;
	std::vector<uint32_t> triangles = grid.Triangles, offsets = grid.Offsets, neighbours = grid.Neighbours;

	// Built with no multiply-add, so this file's own contraction cannot
	// change the inputs.
	std::vector<float> delta(positions.size()), weights(grid.Count);
	for (size_t i = 0; i < delta.size(); ++i) delta[i] = float(int(i % 101) - 50) / 4096.0f;
	for (size_t v = 0; v < weights.size(); ++v) weights[v] = float(v % 37) / 36.0f;

	std::vector<float> blended = positions;
	REQUIRE(mk::Axpy(Span(blended), 0.37f, Span(delta), uint32_t(delta.size())));
	REQUIRE(mk::WeightedAxpy(Span(blended), Span(weights), Span(delta), grid.Count, 3));

	std::vector<float> matrices = {0.9f, 0.1f, 0, 0.2f, -0.1f, 0.95f, 0.05f, 0, 0, 0, 1.1f, -0.3f,
								   1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1, 0};
	std::vector<uint16_t> skinIndices(size_t(grid.Count) * 4);
	std::vector<float> skinWeights(size_t(grid.Count) * 4);
	for (size_t v = 0; v < grid.Count; ++v) {
		skinIndices[v * 4] = 0;
		skinIndices[v * 4 + 1] = 1;
		skinWeights[v * 4] = weights[v];
		skinWeights[v * 4 + 1] = 1.0f - weights[v];
	}
	std::vector<float> skinned(positions.size());
	REQUIRE(mk::Transform(
		Span(skinned), Span(blended), Span(matrices), Span(skinIndices), Span(skinWeights), grid.Count
	));

	std::vector<float> normals(positions.size());
	REQUIRE(mk::Normals(Span(normals), Span(skinned), Span(triangles), std::nullopt, grid.Count));

	std::vector<float> relaxed(positions.size());
	REQUIRE(mk::Laplacian(Span(relaxed), Span(skinned), Span(offsets), Span(neighbours), Span(weights), 4, 0.5f));

	CHECK(Fnv(blended) == 0x7aac34c2522c47dcull);
	CHECK(Fnv(skinned) == 0x91c94e5dd0c8762cull);
	CHECK(Fnv(normals) == 0xca8aec165b9d77c5ull);
	CHECK(Fnv(relaxed) == 0xd021e91a6e0be3f6ull);
}

TEST_CASE("an unaligned stream gives the same bytes as an aligned one", "[script][meshkernels]") {
	// One byte in, so every f32 straddles an alignment boundary.
	std::vector<std::byte> raw(1 + 12), deltaRaw(1 + 12);
	const float start[3] = {1, 2, 3}, delta[3] = {10, 20, 30};
	std::memcpy(raw.data() + 1, start, 12);
	std::memcpy(deltaRaw.data() + 1, delta, 12);
	CHECK(mk::Axpy(mk::Bytes(raw).subspan(1), 0.5f, mk::Bytes(deltaRaw).subspan(1), 3));
	float result[3];
	std::memcpy(result, raw.data() + 1, 12);
	CHECK(result[0] == 6.0f);
	CHECK(result[1] == 12.0f);
	CHECK(result[2] == 18.0f);
}
