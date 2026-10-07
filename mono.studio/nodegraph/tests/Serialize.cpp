// The save format.
//
// **A save format that drops a widget is a file somebody loses work to**, and
// it is silent: the graph loads, draws, and quietly holds a default where a
// number used to be. The round trip is checked by signature rather than
// field-by-field, because the signature is what the cache trusts - so a load
// that changed anything the evaluator can see fails here, and one that changed
// only a position does not, which is correct.

#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <nodegraph/Graph.hpp>
#include <nodegraph/Serialize.hpp>
#include <nodegraph/Types.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("studio.nodegraph.serialize")

using namespace nodegraph;
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

TEST_CASE("instance output interface survives file and template round trips", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;
	const auto source = graph.Add("field.source", 0, 0);
	REQUIRE(graph.SetOutputs(source, {PortSpec{"Extra", "data.FIELD"}}));
	Graph restored;
	std::string failure;
	REQUIRE(Load(Save(graph), restored, failure));
	REQUIRE(restored.Nodes().size() == 1);
	REQUIRE(restored.Nodes()[0].OutputPorts);
	CHECK(restored.Nodes()[0].OutputPorts->size() == 1);
	CHECK(restored.Nodes()[0].OutputPorts->front().Name == "Extra");
	CHECK(restored.Hash(restored.Nodes()[0].Id) == graph.Hash(source));
}

TEST_CASE("empty output override retains its presence after saving", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;
	const auto node = graph.Add("field.source", 0, 0);
	const auto original = graph.Hash(node);
	REQUIRE(graph.SetOutputs(node, {}));
	REQUIRE(graph.Hash(node) != original);
	Graph restored;
	std::string error;
	REQUIRE(Load(Save(graph), restored, error));
	REQUIRE(restored.Nodes().size() == 1);
	REQUIRE(restored.Nodes()[0].OutputPorts);
	CHECK(restored.Nodes()[0].OutputPorts->empty());
	CHECK(restored.Hash(restored.Nodes()[0].Id) == graph.Hash(node));
}

TEST_CASE("input interface overrides survive serialization including empty presence", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;
	const NodeId opaque = graph.Add("field.source", 0, 0);
	const NodeId empty = graph.Add("field.source", 200, 0);
	REQUIRE(opaque != NO_NODE);
	REQUIRE(empty != NO_NODE);
	Node *opaqueNode = graph.Find(opaque);
	Node *emptyNode = graph.Find(empty);
	REQUIRE(opaqueNode);
	REQUIRE(emptyNode);
	opaqueNode->Type = "plugin.future-serialized";
	opaqueNode->InputPorts = std::vector<PortSpec>{PortSpec{"explicit-in", "data.FIELD"}};
	opaqueNode->OutputPorts = std::vector<PortSpec>{PortSpec{"explicit-out", "data.NUMBER"}};
	emptyNode->InputPorts.emplace();
	emptyNode->OutputPorts.emplace();

	const std::string text = Save(graph);
	Graph loaded;
	std::string error;
	REQUIRE(Load(text, loaded, error));
	REQUIRE(loaded.Nodes().size() == 2);
	REQUIRE(loaded.Nodes()[0].InputPorts);
	REQUIRE(loaded.Nodes()[0].OutputPorts);
	REQUIRE(loaded.Nodes()[0].InputPorts->size() == 1);
	REQUIRE(loaded.Nodes()[0].OutputPorts->size() == 1);
	CHECK(loaded.Nodes()[0].InputPorts->front().Name == "explicit-in");
	CHECK(loaded.Nodes()[0].InputPorts->front().Type == "data.FIELD");
	CHECK(loaded.Nodes()[0].OutputPorts->front().Name == "explicit-out");
	CHECK(loaded.Nodes()[0].OutputPorts->front().Type == "data.NUMBER");
	CHECK(loaded.Nodes()[1].InputPorts.has_value());
	CHECK(loaded.Nodes()[1].InputPorts->empty());
	CHECK(loaded.Nodes()[1].OutputPorts.has_value());
	CHECK(loaded.Nodes()[1].OutputPorts->empty());
	CHECK(loaded.Hash(loaded.Nodes()[0].Id) == graph.Hash(opaque));
	CHECK(loaded.Hash(loaded.Nodes()[1].Id) == graph.Hash(empty));
	const std::string remappedSave = Save(loaded);
	Graph reloaded;
	REQUIRE(Load(remappedSave, reloaded, error));
	CHECK(Save(reloaded) == remappedSave);
	REQUIRE(reloaded.Nodes()[0].InputPorts);
	CHECK(reloaded.Nodes()[0].InputPorts->front().Name == "explicit-in");
	CHECK(reloaded.Nodes()[0].InputPorts->front().Type == "data.FIELD");
}

TEST_CASE("registered union ports survive dynamic port save and load", "[nodegraph][union]") {
	RegisterFixtureNodes();
	DataType mixed;
	mixed.Id = "fixture.union.saved";
	mixed.Members = {"data.FIELD", "data.NUMBER"};
	DataTypes::Register(mixed);
	Graph graph;
	const auto source = graph.Add("field.source", 0, 0);
	const auto sink = graph.Add("field.blend", 260, 0);
	REQUIRE(graph.SetDynamicInputs(sink, {PortSpec{"Extra", mixed.Id, false}}));
	REQUIRE(graph.SetOutputs(source, {Port("Out", mixed.Id)}));
	REQUIRE(graph.Connect(source, "Out", sink, "Extra") == LinkResult::Made);
	const auto text = Save(graph);
	Graph restored;
	std::string error;
	REQUIRE(Load(text, restored, error));
	CHECK(Save(restored) == text);
	REQUIRE(restored.Find(sink)->DynamicInputs.size() == 1);
	CHECK_FALSE(restored.Find(sink)->DynamicInputs.front().Suggest);
	CHECK(restored.CanConnect(source, "Out", sink, "Extra") == LinkResult::Made);
	CHECK(restored.CanConnect(source, "Out", sink, "A") == LinkResult::TypeMismatch);
}
