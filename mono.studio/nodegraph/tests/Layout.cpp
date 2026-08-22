// Where a node's ports and knobs land.
//
// **One layout, three consumers** - the painter, the hit test and the
// inspector. What is checked here is that everything a node claims to have is
// inside the body that will be drawn for it, because a widget placed where it
// cannot be clicked is the failure that arrangement exists to make impossible.

#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Graph.hpp>
#include <nodegraph/Layout.hpp>

TEST_SUITE_ID("studio.nodegraph.layout")

using namespace nodegraph;
using fixture::RegisterFixtureNodes;

TEST_CASE("layout puts every port and widget inside the node", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId blend = graph.Add("field.blend", 0.0f, 0.0f);
	REQUIRE(blend != NO_NODE);
	const NodeLayout layout = LayoutOf(*graph.Find(blend));

	// Four inputs and one output, and two knobs.
	CHECK(layout.Ports.size() == 5);
	CHECK(layout.Widgets.size() == 2);

	// Its output wire has a picture, so the layout reserved a square for one.
	// **The preview belongs to `data.FIELD`, not to this node** - which is what
	// makes a reserved square a property of what the wire carries.
	CHECK(layout.PreviewSide > 0.0f);
	CHECK(layout.PreviewTop > 0.0f);
	CHECK(layout.PreviewTop + layout.PreviewSide <= layout.Height);

	for (const PlacedPort &port : layout.Ports) {
		CHECK(port.Y > 0.0f);
		CHECK(port.Y < layout.Height);
		CHECK(port.X == (port.Input ? 0.0f : layout.Width));
	}
	for (const PlacedWidget &widget : layout.Widgets) {
		CHECK(widget.Y >= layout.WidgetsTop);
		CHECK(widget.Y + widget.Height <= layout.Height);
		CHECK(widget.X + widget.Width <= layout.Width);
	}

	CHECK(PortIn(layout, "A", true) != nullptr);
	CHECK(PortIn(layout, "A", false) == nullptr);
}

TEST_CASE("a node whose type is missing still has a body", "[nodegraph]") {
	RegisterFixtureNodes();

	// **A document naming a type this build does not have is the ordinary
	// case**, not a corruption: it is a graph saved by a build with one more
	// plugin in it. It has to be visible, movable and deletable, so it gets a
	// body and no ports rather than nothing at all.
	Node stranger;
	stranger.Type = "somebody.plugin";
	const NodeLayout broken = LayoutOf(stranger);
	CHECK(broken.Width > 0.0f);
	CHECK(broken.Height > 0.0f);
	CHECK(broken.Ports.empty());
}

TEST_CASE("a collapsed node keeps its ports and loses its body", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId blend = graph.Add("field.blend", 0.0f, 0.0f);
	const NodeId source = graph.Add("field.source", 0.0f, 300.0f);
	const NodeLayout open = LayoutOf(*graph.Find(blend));

	graph.Find(blend)->Collapsed = true;
	const NodeLayout shut = LayoutOf(*graph.Find(blend));

	// A graph that stopped being readable when it was tidied would be the
	// opposite of tidy, so every wire still has somewhere to land.
	CHECK(shut.Ports.size() == 5);
	CHECK(shut.Widgets.empty());
	CHECK(shut.PreviewSide == 0.0f);
	CHECK(shut.Height < open.Height);

	// And an uncollapsed neighbour is untouched.
	CHECK(LayoutOf(*graph.Find(source)).Widgets.size() == 2);
}
