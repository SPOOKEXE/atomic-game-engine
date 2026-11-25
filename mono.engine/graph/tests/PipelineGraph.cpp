#include <engine/ang/Compile.hpp>
#include <engine/graph/PipelineGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.graph.pipelinegraph")
TEST_DEPENDS("engine.ang.graph")

using namespace engine;

TEST_CASE("the default pipeline reads as a legal node graph", "[graph][pipelinegraph]") {
	ang::Registry types;
	ang::Graph graph;
	std::string error;
	graph::PipelineSet set;
	CHECK(graph::PipelineGraphNames(set) == std::vector<std::string>{"Default PBR"});
	const bool opened = graph::OpenPipelineGraph(set, "Default PBR", types, graph, error);
	INFO(error);
	REQUIRE(opened);
	CHECK(graph.Nodes().size() > 10);
	CHECK(graph.Links().size() > 10);
	ang::Plan plan;
	const ang::CompileResult result = ang::Compile(graph, types, {.Runs = ang::Domain::Frame}, plan);
	for (const ang::Diagnostic &diagnostic : result.Diagnostics) {
		UNSCOPED_INFO(diagnostic.Node << "." << diagnostic.Port << ": " << diagnostic.Message);
	}
	CHECK(result.Ok);
	CHECK(!graph::OpenPipelineGraph(set, "missing", types, graph, error));
	CHECK(graph.Nodes().size() > 10);
}
