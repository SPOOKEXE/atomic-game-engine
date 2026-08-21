// The save format.
//
// **A save format that drops a widget is a file somebody loses work to**, and
// it is silent: the graph loads, draws, and quietly holds a default where a
// number used to be. The round trip is checked by signature rather than
// field-by-field, because the signature is what the cache trusts - so a load
// that changed anything the evaluator can see fails here, and one that changed
// only a position does not, which is correct.

#include "Fixture.hpp"

#include <engine/nodegraph/Graph.hpp>
#include <engine/nodegraph/Serialize.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

TEST_SUITE_ID("engine.nodegraph.serialize")

using namespace engine::nodegraph;
using fixture::RegisterFixtureNodes;

TEST_CASE("a graph survives a save and a load", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 40.0f, 60.0f);
	const NodeId blend = graph.Add("field.blend", 300.0f, 60.0f);
	graph.Find(source)->Widgets["frequency"].Number = 7.5;
	graph.Find(source)->Widgets["resolution"].Text = "256";
	graph.Find(blend)->Label = "to disk";
	REQUIRE(graph.Connect(source, "Out", blend, "A") == LinkResult::Made);

	const std::string text = Save(graph);
	const uint64_t before = graph.Signature();

	Graph loaded;
	std::string error;
	REQUIRE(Load(text, loaded, error));
	CHECK(error.empty());
	CHECK(loaded.Nodes().size() == 2);
	CHECK(loaded.Links().size() == 1);

	CHECK(loaded.Signature() == before);

	// **Byte-identical on the way back out.** A format that re-serialises
	// differently from what it read makes every diff of a saved graph unusable,
	// and the difference is usually a default that was written where an absent
	// value should have been.
	CHECK(Save(loaded) == text);

	// The knobs came back as themselves rather than as their defaults.
	const Node *reloaded = nullptr;
	for (const Node &one : loaded.Nodes()) {
		if (one.Type == "field.source") {
			reloaded = &one;
		}
	}
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded->Widgets.at("frequency").Number == 7.5);
	CHECK(reloaded->Widgets.at("resolution").Text == "256");
}

TEST_CASE("a bad document is refused rather than half-loaded", "[nodegraph]") {
	RegisterFixtureNodes();

	Graph loaded;
	std::string error;

	// **An error string, not a throw and not a silent empty graph.** The caller
	// is an editor opening a file somebody chose, so "which file and why" is the
	// whole of what it needs.
	CHECK(!Load("this is not a graph", loaded, error));
	CHECK(!error.empty());

	CHECK(!Load("", loaded, error));
}

TEST_CASE("a graph naming an unknown type still loads", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 0.0f, 0.0f);
	REQUIRE(source != NO_NODE);
	const std::string text = Save(graph);

	// Rewriting the type id is what a build with one fewer plugin sees. It has
	// to arrive as a node with no evaluation - `Skipped`, and visible - rather
	// than as a refused document, or one missing plugin would cost the whole
	// file.
	std::string mangled = text;
	const size_t at = mangled.find("field.source");
	REQUIRE(at != std::string::npos);
	mangled.replace(at, std::string("field.source").size(), "field.absent");

	Graph loaded;
	std::string error;
	REQUIRE(Load(mangled, loaded, error));
	CHECK(loaded.Nodes().size() == 1);
	CHECK(loaded.Nodes().front().Type == "field.absent");
}
