#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Graph.hpp>
#include <nodegraph/Registry.hpp>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph_noise")
TEST_DEPENDS("engine.imagegraph.noise_field")

TEST_CASE("Noise dimension edits preserve field IDs and canvas union connections", "[studio][noise_field]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"field", "value.noise_field", "", {}, {{"dimension", EnumValue{2}}}},
		{"sample", "value.sample_noise", "", {}, {{"position", Vector2{}}}},
		{"number", "pc.number", "", {}, {}}
	};
	document.Links = {{"field", "field", "sample", "field"}};
	document.Outputs = {{"out", "sample", "value"}};
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	Diagnostic diagnostic;
	for (int64_t dimension : {1, 2, 3}) {
		REQUIRE(studio::SetImageGraphValue(document, "field", "dimension", EnumValue{dimension}, diagnostic));
		Value position = dimension == 1	  ? Value{.25}
						 : dimension == 2 ? Value{Vector2{.25, .5}}
										  : Value{Vector3{.25, .5, .75}};
		REQUIRE(studio::SetImageGraphValue(document, "sample", "position", position, diagnostic));
		REQUIRE(studio::LoadImageGraphCanvas(document, graph, ids, error));
		CHECK(graph.LinkInto(ids.ToCanvas.at("sample"), "field") != nullptr);
		CHECK(
			graph.CanConnect(ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("sample"), "position") ==
			nodegraph::LinkResult::Made
		);
		CHECK(
			graph.CanConnect(ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("sample"), "field") ==
			nodegraph::LinkResult::TypeMismatch
		);
		Document saved;
		REQUIRE(studio::SaveImageGraphCanvas(graph, document, ids, saved, error));
		CHECK(saved == document);
		Document loaded;
		REQUIRE(Read(Write(saved), loaded, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(loaded, plan, diagnostic) == Status::Ok);
		EvaluatedValue result;
		REQUIRE(EvaluateValue(loaded, plan, "out", {}, result, diagnostic) == Status::Ok);
		CHECK(std::holds_alternative<double>(result.Data));
	}
	CHECK_FALSE(studio::SetImageGraphValue(document, "field", "dimension", EnumValue{0}, diagnostic));
	CHECK_FALSE(studio::SetImageGraphValue(document, "sample", "position", std::string("bad"), diagnostic));
}
