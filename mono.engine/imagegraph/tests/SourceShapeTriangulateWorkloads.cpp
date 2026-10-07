#include "fixtures/SourceShapeTriangulateWorkloads.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.imagegraph.source_shape_triangulate_workloads")
TEST_CASE(
	"Shape and weighted triangulation profiling workloads retain independent oracles",
	"[imagegraph][shape_triangulate_workloads]"
) {
	using Fixture = engine::imagegraph::testing::SourceShapeTriangulateFixture;
	const auto kind = GENERATE(Fixture::Kind::Shape3D, Fixture::Kind::Triangulate);
	INFO("workload=" << int(kind));
	Fixture fixture(kind);
	fixture.Run();
	CHECK(fixture.Verify() == fixture.ExpectedHash);
	fixture.CheckBudget();
}
