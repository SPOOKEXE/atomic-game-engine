#include "Tessellation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.tessellation")

using engine::render::GpuTessellationPlan;
using engine::render::GpuTessellationVertex;
using engine::render::TessellationCapacity;
using engine::render::TessellationPlan;

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
