#include "../src/nodes/SourceParticle3DRecipe.hpp"

#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_particle_3d_recipe")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
namespace source_particle3d_recipe_test {
	struct Run {
		Node Authored{"particles", "pc.3_d_particle", "", {}, {}};
		EvaluationRequest Request;
		NodeContext Context{Authored, *FindCatalogueEntry(Authored.Type), Request};
		SourceParticle3DPrepared Prepared;
		Run() {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			for (const auto &input : Context.Entry.Inputs)
				if (auto value = CatalogueDefault(input)) Context.Values.emplace_back(input.Id, *value);
		}
		void RequirePrepared() {
			const bool prepared = PrepareSourceParticle3DControls(Context, Prepared);
			INFO(Context.FailureMessage);
			INFO(Context.FailurePort);
			REQUIRE(prepared);
		}
		void Set(std::string_view port, Value value) {
			for (auto &[id, stored] : Context.Values)
				if (id == port) {
					stored = std::move(value);
					return;
				}
			Context.Values.emplace_back(port, std::move(value));
		}
	};
}
using namespace source_particle3d_recipe_test;

TEST_CASE(
	"Particle 3D preparation reads source defaults and source curves independently of inspector visibility"
) {
	Run run;
	run.RequirePrepared();
	const auto &c = run.Prepared.Controls;
	CHECK(c.SpawnDelay == 4);
	CHECK(c.SpawnAmount == Vector2{2, 2});
	CHECK(c.Lifespan == Vector2{20, 30});
	CHECK(c.Scale == std::array<double, 6>{1, 1, 1, 1, 1, 1});
	CHECK(c.SpeedCurve != nullptr);
	CHECK(c.PathCurve != nullptr);
	REQUIRE(c.Palette.size() == 1);
	CHECK(c.Palette[0] == Colour{255, 255, 255, 255});
	Run curved;
	curved.Set("velocity_curved", true);
	curved.RequirePrepared();
	CHECK(curved.Prepared.Controls.SpeedCurve != nullptr);
}

TEST_CASE("Particle 3D mesh preparation retains group order and raw object coordinates") {
	Run run;
	run.Set("spawn_source", EnumValue{2});
	MeshValue3D mesh;
	auto &data = mesh.Data.emplace();
	data.Parts.resize(2);
	data.Parts[0].Vertices.push_back({{2, 3, 4}});
	data.Parts[1].Vertices.push_back({{-1, -2, -3}});
	run.Set("spawn_mesh", std::move(mesh));
	run.RequirePrepared();
	const auto groups = run.Prepared.Controls.SpawnMeshVertices;
	REQUIRE(groups.size() == 2);
	CHECK(groups[0][0] == Vector3{2, 3, 4});
	CHECK(groups[1][0] == Vector3{-1, -2, -3});
}

TEST_CASE("Particle 3D direct data reads three coordinates and refuses byte shortage") {
	Run run;
	run.Set("spawn_source", EnumValue{3});
	ArrayValue data;
	data.Nested = {{1.0, 2.0, 3.0}};
	run.Set("spawn_data", std::move(data));
	run.RequirePrepared();
	REQUIRE(run.Prepared.Controls.SpawnData.size() == 1);
	CHECK(run.Prepared.Controls.SpawnData[0] == Vector3{1, 2, 3});
	Run refused;
	refused.Context.ByteBudget = 0;
	CHECK_FALSE(PrepareSourceParticle3DControls(refused.Context, refused.Prepared));
	CHECK(refused.Context.FailureCode == Status::LimitExceeded);
}

TEST_CASE("Particle 3D preparation owns planar path adapter for follow sampling") {
	Run run;
	run.Set("follow_path", true);
	Path2D path;
	path.Anchors = {{{0, 0, 0, 0, 0, 0}}, {{1, 1, 0, 0, 0, 0}}};
	run.Set("path", std::move(path));
	run.RequirePrepared();
	CHECK(run.Prepared.Controls.Follow);
	CHECK(run.Prepared.Controls.FollowPath != nullptr);
	CHECK(run.Prepared.FollowPlanar != nullptr);
	CHECK(run.Prepared.Controls.PathSampleWork > 0);
}

TEST_CASE("Particle 3D direct data preserves general item triples and vector3 leaves") {
	Run run;
	run.Set("spawn_source", EnumValue{3});
	ArrayValue data;
	data.Items.push_back({ElementValue{Vector3{4, 5, 6}}});
	data.Items.push_back(
		{std::vector<SourceArrayItem>{{ElementValue{1.0}}, {ElementValue{2.0}}, {ElementValue{3.0}}}}
	);
	run.Set("spawn_data", std::move(data));
	run.RequirePrepared();
	REQUIRE(run.Prepared.Controls.SpawnData.size() == 2);
	CHECK(run.Prepared.Controls.SpawnData[0] == Vector3{4, 5, 6});
	CHECK(run.Prepared.Controls.SpawnData[1] == Vector3{1, 2, 3});
}
