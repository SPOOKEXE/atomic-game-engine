#include "RigidSchedule.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_rigid_schedule")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
TEST_CASE("Rigid owner prefixes preserve compiled actor interleaving", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"owner", "pc.rigid_group_inline", "", {}, {}},
		{"body", "pc.rigid_object", "scope", {}, {}},
		{"render_a", "pc.rigid_render", "scope", {}, {}},
		{"observer", "pc.rigid_variable", "scope", {}, {}},
		{"force", "pc.rigid_force_apply", "scope", {}, {}},
		{"render_b", "pc.rigid_render_id", "scope", {}, {}}
	};
	Group scope{"scope", "scope"};
	scope.OwnerNodeId = "owner";
	document.Groups = {scope};
	document.Links = {{"body", "object", "observer", "objects"}, {"body", "object", "force", "object"}};
	document.Outputs = {{"image", "render_b", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto original = plan.NodeOrder;
	EvaluationBudget budget(65536);
	AllocationReservation charge;
	REQUIRE(AppendRigidSchedule(document, plan, budget, charge, diagnostic) == Status::Ok);
	CHECK(plan.NodeOrder == original);
	std::vector<size_t> expectedActors;
	for (size_t index : original)
		if (index) expectedActors.push_back(index);
	REQUIRE(expectedActors.size() == 5);
	for (size_t i = 1; i < expectedActors.size(); ++i)
		CHECK(
			std::find(
				plan.InlineControlDependencies.begin(),
				plan.InlineControlDependencies.end(),
				InlineControlDependency{expectedActors[i], expectedActors[i - 1]}
			) != plan.InlineControlDependencies.end()
		);
	CHECK(AppendRigidSchedule(document, plan, budget, charge, diagnostic) == Status::Ok);
	CHECK(plan.InlineControlDependencies.size() == 4);
}
