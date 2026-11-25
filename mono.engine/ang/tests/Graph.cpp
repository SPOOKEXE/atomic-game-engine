#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.ang.graph")
TEST_DEPENDS("engine.ang.value")

using namespace engine::ang;

namespace {
	void Undo(Graph &graph, const EditRecord &record) {
		const EditResult undone = graph.Apply(std::span<const Edit>(record.Inverse));
		REQUIRE(undone.Ok);
	}
}

TEST_CASE("removing a node takes its links and undo restores both", "[ang][graph]") {
	Graph graph;
	tests::Add(graph, tests::Node("a", "test.const"));
	tests::Add(graph, tests::Node("b", "test.add"));
	tests::Wire(graph, "a", "value", "b", "a");
	const uint64_t revision = graph.Revision();
	const EditResult removed = graph.Apply(edit::RemoveNode{"a"});
	REQUIRE(removed.Ok);
	CHECK(graph.Nodes().size() == 1);
	CHECK(graph.Links().empty());
	CHECK(graph.Revision() == revision + 1);
	Undo(graph, removed.Record);
	CHECK(graph.Nodes().size() == 2);
	REQUIRE(graph.Links().size() == 1);
	CHECK(graph.Links()[0] == Link{"a", "value", "b", "a"});
}

TEST_CASE("every edit kind inverts", "[ang][graph]") {
	Graph graph;
	tests::Add(graph, tests::Node("a", "test.const", {{"value", Value(1.0)}}));
	REQUIRE(graph.Apply(edit::AddGroup{{.Id = "g", .Label = "Group"}}).Ok);
	const std::vector<Edit> batch{
		edit::MoveNode{"a", {10.0f, 20.0f}},
		edit::SetParam{"a", "value", Value(5.0)},
		edit::SetBypass{"a", true},
		edit::SetNodeGroup{"a", "g"},
		edit::SetGroupFrame{"g", {1.0f, 2.0f}, {300.0f, 200.0f}},
	};
	const EditResult applied = graph.Apply(std::span<const Edit>(batch));
	REQUIRE(applied.Ok);
	const NodeInstance *node = graph.FindNode("a");
	CHECK(node->Position.X == 10.0f);
	CHECK(node->Params.at("value") == Value(5.0));
	CHECK(node->Bypassed);
	CHECK(node->Group == "g");
	CHECK(graph.FindGroup("g")->Size.X == 300.0f);
	Undo(graph, applied.Record);
	CHECK(graph.FindGroup("g")->Size.X == 0.0f);
	node = graph.FindNode("a");
	CHECK(node->Position.X == 0.0f);
	CHECK(node->Params.at("value") == Value(1.0));
	CHECK(!node->Bypassed);
	CHECK(node->Group.empty());

	// Removing a group clears membership; undo restores it.
	REQUIRE(graph.Apply(edit::SetNodeGroup{"a", "g"}).Ok);
	const EditResult ungrouped = graph.Apply(edit::RemoveGroup{"g"});
	REQUIRE(ungrouped.Ok);
	CHECK(graph.FindNode("a")->Group.empty());
	Undo(graph, ungrouped.Record);
	CHECK(graph.FindNode("a")->Group == "g");

	// Clearing a param removes it.
	REQUIRE(graph.Apply(edit::SetParam{"a", "value", Value()}).Ok);
	CHECK(!graph.FindNode("a")->Params.contains("value"));
}

TEST_CASE("refused edits and batches leave the graph unchanged", "[ang][graph]") {
	Graph graph;
	tests::Add(graph, tests::Node("a", "test.const"));
	CHECK(!graph.Apply(edit::AddNode{tests::Node("a", "test.const")}).Ok);
	CHECK(!graph.Apply(edit::AddNode{tests::Node("", "test.const")}).Ok);
	CHECK(!graph.Apply(edit::AddLink{{"a", "value", "missing", "in"}}).Ok);
	CHECK(!graph.Apply(edit::RemoveLink{{"a", "value", "a", "in"}}).Ok);
	CHECK(!graph.Apply(edit::SetNodeGroup{"a", "nope"}).Ok);
	const uint64_t revision = graph.Revision();
	const std::vector<Edit> batch{edit::MoveNode{"a", {5.0f, 5.0f}}, edit::RemoveNode{"missing"}};
	CHECK(!graph.Apply(std::span<const Edit>(batch)).Ok);
	CHECK(graph.FindNode("a")->Position.X == 0.0f);
	CHECK(graph.Revision() == revision);
}

TEST_CASE("new ids use the definition stem and skip used ones", "[ang][graph]") {
	Graph graph;
	CHECK(graph.NewNodeId("math.add") == "add1");
	tests::Add(graph, tests::Node("add1", "math.add"));
	CHECK(graph.NewNodeId("math.add") == "add2");
	CHECK(graph.NewNodeId("") == "node1");
}

TEST_CASE("upgrade migrates step by step and refuses without a migrator", "[ang][graph]") {
	Registry registry = Registry::WithScalars();
	REQUIRE(registry
				.DefineNode(
					{.Id = "v3",
					 .Version = 3,
					 .Migrate =
						 [](NodeInstance &node, uint32_t from) {
							 // v1 named the param "size"; v2 renamed it "radius"; v3 doubled it.
							 if (from == 1) {
								 node.Params["radius"] = node.Params["size"];
								 node.Params.erase("size");
							 } else if (from == 2) {
								 node.Params["radius"] = Value(node.Params["radius"].AsNumber() * 2.0);
							 }
							 return true;
						 }}
				)
				.Ok);
	REQUIRE(registry.DefineNode({.Id = "fixed", .Version = 2}).Ok);
	Graph graph;
	NodeInstance old = tests::Node("old", "v3", {{"size", Value(4.0)}});
	old.Version = 1;
	tests::Add(graph, old);
	NodeInstance stuck = tests::Node("stuck", "fixed");
	stuck.Version = 1;
	tests::Add(graph, stuck);
	tests::Add(graph, tests::Node("fresh", "v3"));
	std::vector<std::string> refused;
	CHECK(graph.Upgrade(registry, &refused) == 1);
	CHECK(graph.FindNode("old")->Version == 3);
	CHECK(graph.FindNode("old")->Params.at("radius").AsNumber() == 8.0);
	CHECK(graph.FindNode("fresh")->Version == 3);
	CHECK(graph.FindNode("stuck")->Version == 1);
	CHECK(refused == std::vector<std::string>{"stuck"});
}
