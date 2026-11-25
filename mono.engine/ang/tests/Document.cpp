// A graph document written as text reads back the same, and bad text changes nothing.

#include <engine/ang/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.ang.document")
TEST_DEPENDS("engine.ang.graph")

using namespace engine;
using namespace engine::ang;

TEST_CASE("every scalar, link and group survives the text form", "[ang][document]") {
	Graph graph;
	NodeInstance node{.Id = "a\n\"b", .Def = "math.add", .Version = 3, .Position = {1.25f, -7.1f}};
	node.Params = {
		{"bool", Value(true)},
		{"int", Value(int64_t{-9007199254740993})},
		{"number", Value(0.1)},
		{"v2", Value(core::Vector2{1.0f, 0.3f})},
		{"v3", Value(core::Vector3{1.0f, 2.0f, 0.1f})},
		{"v4", Value(Vector4{1.0f, 2.0f, 3.0f, 0.7f})},
		{"colour", Value(core::Color3{0.1f, 0.2f, 0.3f})},
		{"text", Value("line\nnext")},
		{"name", Value(core::Name("thing"))},
		{"tagged", Value(2.0).WithType("meters")},
	};
	node.Bypassed = node.Collapsed = true;
	node.Group = "frame";
	const std::vector<Link> links{{node.Id, "sum", "z", "a"}};
	REQUIRE(graph
				.Load(
					{node, {.Id = "z", .Def = "unknown"}},
					links,
					{{.Id = "frame", .Label = "Frame", .Position = {1, 2}, .Size = {3, 4}}}
				)
				.Ok);
	std::string text, error, again;
	REQUIRE(WriteGraph(graph, text, error));
	Graph read;
	INFO(text);
	REQUIRE(ReadGraph(text, read, error));
	REQUIRE(WriteGraph(read, again, error));
	CHECK(text == again);
	const NodeInstance *back = read.FindNode(node.Id);
	REQUIRE(back != nullptr);
	CHECK(back->Params == node.Params);
	CHECK((back->Bypassed && back->Collapsed && back->Version == 3 && back->Group == "frame"));
	CHECK(read.Links()[0] == links[0]);
	CHECK(read.FindGroup("frame")->Size.Y == 4.0f);
}

TEST_CASE("a refused document leaves the graph alone", "[ang][document]") {
	Graph graph;
	REQUIRE(graph.Apply(edit::AddNode{{.Id = "keep", .Def = "x"}}).Ok);
	const uint64_t revision = graph.Revision();
	std::string error;
	for (const char *text :
		 {"",
		  "other 1\n",
		  "ang 1\nparam \"x\" bool \"bool\" yes\n",
		  "ang 1\nnode \"a\" \"d\" 0 0 inf \"\" no no\n",
		  "ang 1\nnode \"a\" \"d\" 0 0 0 \"missing\" no no\n",
		  "ang 1\nlink \"a\" \"b\" \"c\" \"d\"\n",
		  "ang 1\nnode \"a\" \"d\" 0 0 0 \"\" no no extra\n"}) {
		INFO(text);
		CHECK(!ReadGraph(text, graph, error));
		CHECK(graph.Revision() == revision);
	}
	CHECK(!ReadGraph("ang 1\n", graph, error, {.MaximumBytes = 2}));
	std::string out;
	REQUIRE(graph.Apply(edit::SetParam{"keep", "b", Value::FromBlob("image", {.Identity = 1})}).Ok);
	CHECK(!WriteGraph(graph, out, error));
}
