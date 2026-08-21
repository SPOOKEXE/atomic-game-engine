// The model: what may connect to what, what a cycle is, and what a hash covers.
//
// **This is the half where a mistake is silent.** A canvas that draws a node in
// the wrong place is visible in a second. A link validator that lets a cycle
// through is a hang inside `Ordered`; a hash that never settles is a graph that
// recomputes for ever; a hash that covers too much is a graph that recomputes
// every time somebody tidies it up. None of those look like anything.

#include "Fixture.hpp"

#include <engine/nodegraph/Graph.hpp>
#include <engine/nodegraph/Registry.hpp>
#include <engine/nodegraph/Types.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.nodegraph.graph")

using namespace engine::nodegraph;
using fixture::RegisterFixtureNodes;

TEST_CASE("a type id is the compatibility rule", "[nodegraph]") {
	RegisterFixtureNodes();

	CHECK(DataTypes::CanConnect("data.FIELD", "data.FIELD"));
	CHECK(!DataTypes::CanConnect("data.FIELD", "data.NUMBER"));
	CHECK(DataTypes::CanConnect("data.FIELD", ANY_TYPE));

	// **An unregistered id is not a wildcard.** A typo in a port's type would
	// otherwise connect to everything, which is the worst available reading of
	// a mistake.
	CHECK(!DataTypes::CanConnect("data.TYPO", "data.FIELD"));
	CHECK(!DataTypes::CanConnect("", "data.FIELD"));
}

TEST_CASE("a link is refused for a specific reason", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 0.0f, 0.0f);
	const NodeId blend = graph.Add("field.blend", 260.0f, 0.0f);
	const NodeId number = graph.Add("number.constant", 0.0f, 240.0f);
	REQUIRE(source != NO_NODE);

	CHECK(graph.Connect(source, "Out", blend, "A") == LinkResult::Made);
	CHECK(graph.Connect(number, "Out", blend, "A") == LinkResult::TypeMismatch);
	CHECK(graph.Connect(number, "Out", blend, "Amount") == LinkResult::Made);
	CHECK(graph.Connect(source, "Out", blend, "Nope") == LinkResult::NoSuchPort);
	CHECK(graph.Connect(source, "Out", source, "Frequency") == LinkResult::SameNode);

	// An input takes one link, and the newer one wins.
	const NodeId second = graph.Add("field.source", 0.0f, 480.0f);
	CHECK(graph.Connect(second, "Out", blend, "A") == LinkResult::Made);
	REQUIRE(graph.LinkInto(blend, "A") != nullptr);
	CHECK(graph.LinkInto(blend, "A")->From == second);
	CHECK(graph.Links().size() == 2);

	// An unregistered type is refused rather than placed as a mystery.
	CHECK(graph.Add("nobody.registered.this", 0.0f, 0.0f) == NO_NODE);
}

TEST_CASE("a cycle is refused before it exists", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId one = graph.Add("field.blend", 0.0f, 0.0f);
	const NodeId two = graph.Add("field.blend", 200.0f, 0.0f);
	const NodeId three = graph.Add("field.blend", 400.0f, 0.0f);

	CHECK(graph.Connect(one, "Out", two, "A") == LinkResult::Made);
	CHECK(graph.Connect(two, "Out", three, "A") == LinkResult::Made);

	// **The long way round is the one a shallow check misses**, and a cycle that
	// got in would be a hang inside `Ordered` rather than a wrong picture.
	CHECK(graph.Connect(three, "Out", one, "A") == LinkResult::WouldCycle);
	CHECK(graph.Connect(two, "Out", one, "A") == LinkResult::WouldCycle);
	CHECK(graph.Links().size() == 2);

	// Removing a node takes its links with it, or every later walk has to guard
	// against an endpoint that is not there.
	CHECK(graph.Remove(two));
	CHECK(graph.Links().empty());
}

TEST_CASE("a hash covers parameters and inputs and nothing else", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 0.0f, 0.0f);
	const NodeId blend = graph.Add("field.blend", 200.0f, 0.0f);
	REQUIRE(graph.Connect(source, "Out", blend, "A") == LinkResult::Made);

	const uint64_t before = graph.Hash(blend);

	// Moving or renaming a node invalidates nothing. A hash that included
	// position would recompute the whole graph every time somebody tidied it.
	graph.Find(blend)->X += 40.0f;
	graph.Find(blend)->Label = "renamed";
	CHECK(graph.Hash(blend) == before);

	// An upstream edit invalidates downstream.
	graph.Find(source)->Widgets["frequency"].Number = 9.0;
	CHECK(graph.Hash(blend) != before);

	// A sideways edit does not touch a node that does not read it.
	const NodeId lonely = graph.Add("field.source", 0.0f, 400.0f);
	const uint64_t lonelyBefore = graph.Hash(lonely);
	graph.Find(blend)->Widgets["amount"].Number = 0.25;
	CHECK(graph.Hash(lonely) == lonelyBefore);

	// **It settles.** Asking twice with nothing changed has to answer the same,
	// or every cache lookup misses for ever.
	CHECK(graph.Hash(blend) == graph.Hash(blend));
	CHECK(graph.Signature() == graph.Signature());
}

TEST_CASE("frames hold members, and neither they nor collapsing change a hash", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 40.0f, 60.0f);
	const NodeId ridged = graph.Add("field.ridged", 40.0f, 340.0f);
	const NodeId blend = graph.Add("field.blend", 300.0f, 60.0f);
	REQUIRE(graph.Connect(source, "Out", blend, "A") == LinkResult::Made);
	REQUIRE(graph.Connect(ridged, "Out", blend, "B") == LinkResult::Made);

	const uint64_t settled = graph.Signature();

	// **Neither is a parameter.** A frame is where somebody put a rectangle and
	// collapsing is a node somebody has finished reading. If either reached the
	// hash, tidying a graph would recompute it.
	const GroupId frame = graph.Group({source, ridged}, "Sources", Colour::Hex(0x4ADE80));
	REQUIRE(frame != NO_GROUP);
	graph.Find(blend)->Collapsed = true;
	CHECK(graph.Signature() == settled);

	CHECK(graph.GroupOf(source) == frame);
	CHECK(graph.GroupOf(blend) == NO_GROUP);

	// A node joining a second frame leaves the first, so one drag never moves it
	// twice.
	const GroupId second = graph.Group({ridged, blend}, "Rest", Colour::Hex(0x38BDF8));
	CHECK(graph.GroupOf(ridged) == second);
	REQUIRE(graph.FindGroup(frame) != nullptr);
	CHECK(graph.FindGroup(frame)->Members.size() == 1);

	// Removing a node takes it out of its frame, so no member id ever names
	// something that is not there.
	REQUIRE(graph.Remove(source));
	for (const Group &held : graph.Groups()) {
		for (const NodeId member : held.Members) {
			CHECK(graph.Alive(member));
		}
	}
}

TEST_CASE("compression moves no link and no hash", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	//   source ──▶ warp ──▶ terrace ──▶ readout
	//   ridged ──▶ warp(By)
	const NodeId source = graph.Add("field.source", 0.0f, 0.0f);
	const NodeId ridged = graph.Add("field.ridged", 0.0f, 300.0f);
	const NodeId warp = graph.Add("field.warp", 300.0f, 0.0f);
	const NodeId terrace = graph.Add("field.terrace", 600.0f, 0.0f);
	const NodeId readout = graph.Add("field.readout", 900.0f, 0.0f);

	REQUIRE(graph.Connect(source, "Out", warp, "In") == LinkResult::Made);
	REQUIRE(graph.Connect(ridged, "Out", warp, "By") == LinkResult::Made);
	REQUIRE(graph.Connect(warp, "Out", terrace, "In") == LinkResult::Made);
	REQUIRE(graph.Connect(terrace, "Out", readout, "In") == LinkResult::Made);

	const size_t wires = graph.Links().size();
	std::vector<std::pair<NodeId, uint64_t>> hashes;
	for (const Node &one : graph.Nodes()) {
		hashes.emplace_back(one.Id, graph.Hash(one.Id));
	}

	const NodeId folded = graph.Compress({warp, terrace}, 450.0f, 0.0f);
	REQUIRE(folded != NO_NODE);

	// **No link was re-pointed and no content hash moved.** That is the whole
	// design: the evaluator, the cycle guard and the cache never learn that
	// compression happened, so none of them can be wrong about it - and folding
	// a chain therefore recomputes nothing.
	CHECK(graph.Links().size() == wires);
	for (const auto &[id, was] : hashes) {
		CHECK(graph.Hash(id) == was);
	}
	CHECK(graph.Contents(folded).size() == 2);
	CHECK(graph.Find(warp)->Owner == folded);
	CHECK(graph.Find(source)->Owner == NO_NODE);

	// Expanding is the exact inverse.
	REQUIRE(graph.Expand(folded));
	CHECK(graph.Find(warp)->Owner == NO_NODE);
	CHECK(graph.Links().size() == wires);
	CHECK(!graph.Alive(folded));

	// And deleting a fold takes its contents, so nothing is left that no view
	// can reach.
	const NodeId again = graph.Compress({warp, terrace}, 450.0f, 0.0f);
	REQUIRE(again != NO_NODE);
	REQUIRE(graph.Remove(again));
	CHECK(!graph.Alive(warp));
	CHECK(!graph.Alive(terrace));
	CHECK(graph.Alive(source));
}
