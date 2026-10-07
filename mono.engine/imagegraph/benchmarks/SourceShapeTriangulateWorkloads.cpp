#include "../tests/fixtures/SourceShapeTriangulateWorkloads.hpp"

#include "SourceWorkloadProfile.hpp"

TEST_SUITE_ID("engine.imagegraph.bench.shape-triangulate")
using Fixture = engine::imagegraph::testing::SourceShapeTriangulateFixture;
BENCH("CPU Shape3D Octahedron 64x64 surface and rim compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::Shape3D);
	p.Measure();
}
BENCH("CPU Weighted Triangulate five concave points compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::Triangulate);
	p.Measure();
}
