// Editor undo over ang edit records: undo applies a record's inverse, redo the
// record that inverse produced, and a field edit can fold into one step.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/GraphHistory.hpp>

TEST_SUITE_ID("studio.graphhistory")
TEST_DEPENDS("engine.ang.graph")

namespace ang = engine::ang;

TEST_CASE("graph history undoes, redoes and folds edits through their records", "[studio]") {
	ang::Graph graph;
	studio::GraphHistory history;
	history.Record(graph.Apply(ang::edit::AddNode{{.Id = "a", .Def = "test"}}).Record);
	history.Record(graph.Apply(ang::edit::SetParam{"a", "size", ang::Value(1.0)}).Record);
	// A second change to the same field folds in: one undo returns to before both.
	history.Amend(graph.Apply(ang::edit::SetParam{"a", "size", ang::Value(2.0)}).Record);
	CHECK(graph.FindNode("a")->Params.at("size") == ang::Value(2.0));

	REQUIRE(history.Undo(graph));
	CHECK(!graph.FindNode("a")->Params.contains("size"));
	REQUIRE(history.Redo(graph));
	CHECK(graph.FindNode("a")->Params.at("size") == ang::Value(2.0));
	REQUIRE(history.Undo(graph));
	REQUIRE(history.Undo(graph));
	CHECK(graph.Nodes().empty());
	CHECK(!history.CanUndo());

	// A new step forgets redo.
	history.Record(graph.Apply(ang::edit::AddNode{{.Id = "b", .Def = "test"}}).Record);
	CHECK(!history.CanRedo());

	// A graph replaced underneath refuses the inverse and the history clears.
	graph = {};
	CHECK(!history.Undo(graph));
	CHECK(!history.CanUndo());
	CHECK(!history.CanRedo());
}
