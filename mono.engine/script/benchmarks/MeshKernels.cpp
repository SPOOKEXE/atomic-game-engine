// `MeshKernels` over 25,600 vertices (a 160 x 160 grid), one call per
// iteration. The target is under 1 ms per call on the dev build: an Anatomy
// deformer stack runs ten to twenty of these inside a 50 ms edit budget.

#include <engine/script/MeshKernels.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.script.bench.meshkernels")

namespace mk = engine::script::mesh_kernels;

namespace {
	template <typename T> mk::Bytes Span(std::vector<T> &values) {
		return {reinterpret_cast<std::byte *>(values.data()), values.size() * sizeof(T)};
	}

	constexpr uint32_t SIDE = 160;
	constexpr uint32_t COUNT = SIDE * SIDE;

	struct Mesh {
		std::vector<float> Positions, Out, Delta, Weights, Matrices, SkinWeights, Display;
		std::vector<uint16_t> SkinIndices;
		std::vector<uint32_t> Triangles, Offsets, Neighbours, Weld, Remap;

		Mesh() {
			for (uint32_t y = 0; y < SIDE; ++y) {
				for (uint32_t x = 0; x < SIDE; ++x) {
					Positions.insert(
						Positions.end(), {float(x) / 100.0f, float(y) / 100.0f, float((x ^ y) % 17) / 1000.0f}
					);
				}
			}
			Out.resize(Positions.size());
			Delta.assign(Positions.size(), 0.001f);
			for (uint32_t v = 0; v < COUNT; ++v)
				Weights.push_back(float(v % 37) / 36.0f);
			Matrices.assign(24 * 4, 0.0f);
			for (size_t m = 0; m < 4; ++m) {
				for (size_t r = 0; r < 3; ++r)
					Matrices[m * 12 + r * 5] = 1.0f;
			}
			for (uint32_t v = 0; v < COUNT; ++v) {
				SkinIndices.insert(SkinIndices.end(), {0, 1, 2, 3});
				SkinWeights.insert(SkinWeights.end(), {0.4f, 0.3f, 0.2f, 0.1f});
			}
			for (uint32_t y = 0; y + 1 < SIDE; ++y) {
				for (uint32_t x = 0; x + 1 < SIDE; ++x) {
					const uint32_t a = y * SIDE + x, b = a + 1, c = a + SIDE, d = c + 1;
					Triangles.insert(Triangles.end(), {a, b, c, b, d, c});
				}
			}
			Offsets.push_back(0);
			for (uint32_t y = 0; y < SIDE; ++y) {
				for (uint32_t x = 0; x < SIDE; ++x) {
					const uint32_t v = y * SIDE + x;
					if (x > 0) Neighbours.push_back(v - 1);
					if (x + 1 < SIDE) Neighbours.push_back(v + 1);
					if (y > 0) Neighbours.push_back(v - SIDE);
					if (y + 1 < SIDE) Neighbours.push_back(v + SIDE);
					Offsets.push_back(uint32_t(Neighbours.size()));
				}
			}
			for (uint32_t v = 0; v < COUNT; ++v)
				Weld.push_back(v);
			for (uint32_t v = 0; v < COUNT; ++v)
				Remap.push_back((v * 7919u) % COUNT);
			Display.resize(Positions.size());
		}
	};

	Mesh &Fixture() {
		static Mesh mesh;
		return mesh;
	}
}

BENCH("MeshKernels.Axpy, 25600 vertices x 3", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(mk::Axpy(Span(m.Out), 0.5f, Span(m.Delta), COUNT * 3) ? 1 : 0);
}

BENCH("MeshKernels.WeightedAxpy, 25600 vertices", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(mk::WeightedAxpy(Span(m.Out), Span(m.Weights), Span(m.Delta), COUNT, 3) ? 1 : 0);
}

BENCH("MeshKernels.Transform, 25600 vertices, 4 matrices", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(
		mk::Transform(
			Span(m.Out), Span(m.Positions), Span(m.Matrices), Span(m.SkinIndices), Span(m.SkinWeights), COUNT
		)
			? 1
			: 0
	);
}

BENCH("MeshKernels.Normals, 25600 vertices, welded", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(
		mk::Normals(Span(m.Out), Span(m.Positions), Span(m.Triangles), Span(m.Weld), COUNT) ? 1 : 0
	);
}

BENCH("MeshKernels.Laplacian, 25600 vertices, 1 iteration", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(
		mk::Laplacian(
			Span(m.Out), Span(m.Positions), Span(m.Offsets), Span(m.Neighbours), Span(m.Weights), 1, 0.5f
		)
			? 1
			: 0
	);
}

BENCH("MeshKernels.Gather, 25600 vertices x 3", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(mk::Gather(Span(m.Display), Span(m.Positions), Span(m.Remap), 3) ? 1 : 0);
}

BENCH("MeshKernels.Scatter, 25600 vertices x 3", 1) {
	Mesh &m = Fixture();
	engine::testing::Consume(mk::Scatter(Span(m.Display), Span(m.Positions), Span(m.Remap), 3) ? 1 : 0);
}
