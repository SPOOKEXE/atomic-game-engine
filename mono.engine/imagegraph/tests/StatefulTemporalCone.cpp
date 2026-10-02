#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.stateful_temporal_cone")
using namespace engine::imagegraph;

TEST_CASE(
	"Temporal cone includes compiled allocation and inline owners but ignores unrelated nodes",
	"[imagegraph][stateful_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"static", "value.number", "", {}, {}},
		{"cache", "pc.interlaced", "", {}, {}},
		{"sim", "image.verlet_simple", "", {}, {}}
	};
	document.Outputs = {{"static", "static", "value"}};
	Plan plan;
	const std::array<std::string, 1> outputs{"static"};
	auto cone = AnalyzeStatefulTemporalCone(document, plan, outputs);
	CHECK(cone.Valid);
	CHECK(cone.Nodes == 1);
	CHECK(cone.SurfaceCaches == 0);
	CHECK_FALSE(cone.Simulation);
	plan.GroupSurfaceDependencies = {{0, 1, "surface_in"}};
	plan.InlineOwnerDependencies = {{1, 2}};
	cone = AnalyzeStatefulTemporalCone(document, plan, outputs);
	CHECK(cone.Valid);
	CHECK(cone.Nodes == 3);
	CHECK(cone.SurfaceCaches == 1);
	CHECK(cone.Simulation);
	plan.InlineOwnerDependencies = {{1, 2, true}};
	cone = AnalyzeStatefulTemporalCone(document, plan, outputs);
	CHECK_FALSE(cone.Simulation);
	plan.InlineControlDependencies = {{1, 2}};
	CHECK(AnalyzeStatefulTemporalCone(document, plan, outputs).Simulation);
	plan.InlineControlDependencies.clear();
	plan.PcxNamedDependencies = {{1, 2, "Sim.mesh", "mesh", false}};
	CHECK(AnalyzeStatefulTemporalCone(document, plan, outputs).Simulation);
	plan.InlineOwnerDependencies = {{1, 99}};
	CHECK_FALSE(AnalyzeStatefulTemporalCone(document, plan, outputs).Valid);
}
