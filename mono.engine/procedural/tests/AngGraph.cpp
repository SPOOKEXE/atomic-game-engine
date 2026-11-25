#include <engine/ang/Compile.hpp>
#include <engine/procedural/AngGraph.hpp>
#include <engine/procedural/Region.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.procedural.anggraph")
TEST_DEPENDS("engine.ang.graph")

using namespace engine;

TEST_CASE("a recipe reads as a legal node graph", "[procedural][graph]") {
	const procedural::Recipe recipe = procedural::ForestVillageDefaults("village", ~uint64_t{0});
	ang::Graph graph;
	REQUIRE(procedural::ToAngGraph(recipe, graph));
	CHECK(graph.Nodes().size() == recipe.Nodes.size() + 1);
	CHECK(graph.Links().size() == recipe.Links.size());
	ang::Plan plan;
	CHECK(ang::Compile(graph, procedural::RecipeGraphTypes(), {}, plan).Ok);
	// The root seed is past the signed range and is still exact.
	CHECK(*graph.FindNode("recipe")->Params.at("root_seed").Get<std::string>() == "18446744073709551615");
}
