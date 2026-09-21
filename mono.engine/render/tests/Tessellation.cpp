#include "Tessellation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.tessellation")

using engine::render::GpuTessellationPlan;
using engine::render::GpuTessellationVertex;
using engine::render::TessellationCapacity;
using engine::render::TessellationFactor;
using engine::render::TessellationPlan;
using engine::render::TessellationRequest;

TEST_CASE("tessellation buffers have explicit std430-compatible layouts", "[render][tessellation]") {
	CHECK(sizeof(GpuTessellationVertex) == 48);
	CHECK(sizeof(GpuTessellationPlan) == 36);
	CHECK(TessellationPlan::VerticesPerTriangle(1) == 3);
	CHECK(TessellationPlan::VerticesPerTriangle(4) == 15);
	CHECK(TessellationPlan::IndicesPerTriangle(1) == 3);
	CHECK(TessellationPlan::IndicesPerTriangle(4) == 48);
}

TEST_CASE("tessellation plans reserve bounded disjoint output ranges", "[render][tessellation]") {
	TessellationPlan plan;
	const TessellationCapacity capacity{18, 48, 2};
	REQUIRE(plan.Add({12, 3, 7}, 4, 9, 2, capacity));
	REQUIRE(plan.Add({15, 3, 7}, 5, 10, 1, capacity));
	CHECK(plan.VertexCount == 9);
	CHECK(plan.IndexCount == 15);
	CHECK(plan.Entries[0].OutputFirstVertex == 0);
	CHECK(plan.Entries[0].OutputFirstIndex == 0);
	CHECK(plan.Entries[1].OutputFirstVertex == 6);
	CHECK(plan.Entries[1].OutputFirstIndex == 12);
	CHECK(plan.Entries[0].Material == 4);
	CHECK(plan.Entries[0].Instance == 9);
	CHECK_FALSE(plan.Add({18, 3, 7}, 6, 11, 4, capacity));
}

TEST_CASE("tessellation factors follow projected edge density", "[render][tessellation]") {
	CHECK(TessellationFactor(6.0f, 12.0f, 8) == 1);
	CHECK(TessellationFactor(25.0f, 12.0f, 8) == 3);
	CHECK(TessellationFactor(200.0f, 12.0f, 4) == 4);
	CHECK(TessellationFactor(48.0f, 0.0f, 4) == 1);
}

TEST_CASE("capacity reduces every tessellation request together", "[render][tessellation]") {
	const TessellationRequest requests[]{
		{{0, 3, 0}, 2, 7, 2},
		{{3, 3, 0}, 5, 9, 2},
	};
	TessellationPlan plan;
	CHECK_FALSE(engine::render::BuildCompleteTessellationPlan(requests, {5, 24, 2}, plan));
	CHECK(plan.Entries.empty());
	CHECK(plan.VertexCount == 0);
	CHECK(plan.IndexCount == 0);
	bool complete = false;
	for (uint32_t ceiling = 2; ceiling > 0 && !complete; --ceiling) {
		auto reduced = std::array{requests[0], requests[1]};
		for (TessellationRequest &request : reduced)
			request.Factor = std::min(request.Factor, ceiling);
		complete = engine::render::BuildCompleteTessellationPlan(reduced, {11, 24, 2}, plan);
	}
	REQUIRE(complete);
	REQUIRE(plan.Entries.size() == 2);
	CHECK(plan.Entries[0].Material == 2);
	CHECK(plan.Entries[0].Instance == 7);
	CHECK(plan.Entries[0].Factor == 1);
	CHECK(plan.Entries[1].Material == 5);
	CHECK(plan.Entries[1].Instance == 9);
	CHECK(plan.Entries[1].Factor == 1);
}

TEST_CASE("tessellation retains each source material run", "[render][tessellation]") {
	engine::render::MeshEntry mesh;
	mesh.Textures = {engine::core::Name("tessellation.red"), engine::core::Name("tessellation.blue")};
	mesh.Colours = {{{1, 0, 0, 1}}, {{0, 0, 1, 1}}};
	const auto second = engine::render::TessellationMaterialFor(mesh, 1, {});
	CHECK(second.Texture == engine::core::Name("tessellation.blue"));
	CHECK(second.Colour == std::array<float, 4>{0, 0, 1, 1});
	const auto override =
		engine::render::TessellationMaterialFor(mesh, 1, engine::core::Name("tessellation.override"));
	CHECK(override.Texture == engine::core::Name("tessellation.override"));
	CHECK(override.Colour == std::array<float, 4>{0, 0, 1, 1});
	const auto missing =
		engine::render::TessellationMaterialFor(mesh, 3, engine::core::Name("tessellation.override"));
	CHECK(missing.Texture == engine::core::Name("tessellation.override"));
	CHECK(missing.Colour == std::array<float, 4>{1, 1, 1, 1});
}
