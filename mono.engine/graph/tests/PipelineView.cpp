// Where a node editor draws things, checked without an editor.
//
// **The edges are the part worth testing.** Columns are counting; the edge
// derivation is a decision — `RenderGraph` stores no wires, so what a line
// between two boxes *means* is chosen here, and choosing it wrong draws a
// canvas that is true about resources and false about data.

#include <engine/graph/PipelineView.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

TEST_SUITE_ID("engine.graph.pipelineview")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::Band;
using engine::graph::CompiledGraph;
using engine::graph::Describe;
using engine::graph::GraphStatus;
using engine::graph::LayoutPipeline;
using engine::graph::PipelineLayout;
using engine::graph::RenderGraph;
using engine::graph::ResourceId;
using engine::graph::ResourceKind;
using engine::graph::StandardGraph;

namespace {
	PipelineLayout LayoutOf(const RenderGraph &graph) {
		CompiledGraph compiled;
		Name offender;
		REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
		return LayoutPipeline(graph, compiled);
	}

	// Whether an edge exists from one named node to another, over a resource.
	bool Joined(
		const RenderGraph &graph,
		const PipelineLayout &layout,
		std::string_view from,
		std::string_view to,
		std::string_view resource
	) {
		return std::any_of(
			layout.Edges.begin(), layout.Edges.end(), [&](const engine::graph::PlacedEdge &edge) {
				return graph.Find(edge.From)->Name == Name(from) && graph.Find(edge.To)->Name == Name(to) &&
					   edge.Resource == Name(resource);
			}
		);
	}

	size_t EdgesInto(const RenderGraph &graph, const PipelineLayout &layout, std::string_view to) {
		return static_cast<size_t>(std::count_if(
			layout.Edges.begin(), layout.Edges.end(), [&](const engine::graph::PlacedEdge &edge) {
				return graph.Find(edge.To)->Name == Name(to);
			}
		));
	}
}

TEST_CASE("every enabled node is placed, in execution order", "[graph]") {
	const RenderGraph graph = StandardGraph();
	const PipelineLayout layout = LayoutOf(graph);

	REQUIRE(layout.Nodes.size() == 6);
	CHECK(layout.Nodes.front().Name == Name("shadow"));
	CHECK(layout.Nodes.back().Name == Name("interface"));
}

TEST_CASE("nodes are banded the way the frame runs", "[graph]") {
	const RenderGraph graph = StandardGraph();
	const PipelineLayout layout = LayoutOf(graph);

	const auto bandOf = [&layout](std::string_view name) {
		for (const auto &placed : layout.Nodes) {
			if (placed.Name == Name(name)) {
				return placed.Where;
			}
		}
		FAIL("no such node");
		return Band::PerView;
	};

	CHECK(bandOf("shadow") == Band::Shared);
	CHECK(bandOf("opaque") == Band::PerView);
	CHECK(bandOf("interface") == Band::Final);
}

TEST_CASE("columns restart within each band", "[graph]") {
	// **A column is a position within a band, not across the frame.** The three
	// bands are drawn as three groups, so `shadow` and `surface` both sit at
	// the left of their own.
	const RenderGraph graph = StandardGraph();
	const PipelineLayout layout = LayoutOf(graph);

	const auto columnOf = [&layout](std::string_view name) {
		for (const auto &placed : layout.Nodes) {
			if (placed.Name == Name(name)) {
				return placed.Column;
			}
		}
		FAIL("no such node");
		return 0u;
	};

	CHECK(columnOf("shadow") == 0);
	CHECK(columnOf("surface") == 0);
	CHECK(columnOf("opaque") == 1);
	CHECK(columnOf("overlay") == 0);
	CHECK(columnOf("interface") == 1);

	// Wide enough for the widest band, which is the per-view one at three.
	CHECK(layout.Columns == 3);
}

// --- the edges ----------------------------------------------------------------

TEST_CASE("an edge joins a reader to the node that wrote what it reads", "[graph]") {
	const RenderGraph graph = StandardGraph();
	const PipelineLayout layout = LayoutOf(graph);

	CHECK(Joined(graph, layout, "shadow", "surface", "shadow"));
	CHECK(Joined(graph, layout, "shadow", "opaque", "shadow"));
	CHECK(Joined(graph, layout, "surface", "opaque", "surface"));
	CHECK(Joined(graph, layout, "opaque", "transparent", "colour"));
}

TEST_CASE("the edge comes from the last writer, not from every earlier one", "[graph]") {
	// **The whole decision this file exists to make.** `opaque` writes colour
	// and `transparent` writes it again; a fourth pass reading colour sees
	// `transparent`'s version. An editor drawing a line from `opaque` as well
	// would be stating something true about the resource and false about the
	// data.
	RenderGraph graph;
	const ResourceId colour = graph.AddResource({.Name = Name("colour"), .Kind = ResourceKind::Colour});

	graph.AddNode({.Name = Name("opaque"), .Kind = Name("k"), .Writes = {colour}});
	graph.AddNode({.Name = Name("transparent"), .Kind = Name("k"), .Reads = {colour}, .Writes = {colour}});
	graph.AddNode({.Name = Name("readback"), .Kind = Name("k"), .Reads = {colour}, .Writes = {colour}});

	const PipelineLayout layout = LayoutOf(graph);

	CHECK(Joined(graph, layout, "transparent", "readback", "colour"));
	CHECK_FALSE(Joined(graph, layout, "opaque", "readback", "colour"));

	// Exactly one line into it, rather than one per earlier writer.
	CHECK(EdgesInto(graph, layout, "readback") == 1);
}

TEST_CASE("a read-modify-write node is joined to the pass in front of it", "[graph]") {
	// `transparent` reads the colour it also writes. Resolving reads before
	// recording writes is what stops it being joined to itself.
	const RenderGraph graph = StandardGraph();
	const PipelineLayout layout = LayoutOf(graph);

	const bool selfJoined =
		std::any_of(layout.Edges.begin(), layout.Edges.end(), [](const engine::graph::PlacedEdge &edge) {
			return edge.From == edge.To;
		});
	CHECK_FALSE(selfJoined);
}

TEST_CASE("a resource nothing wrote draws no edge", "[graph]") {
	// `Compile` refuses this as `ReadsBeforeWrite`, so a layout only meets it
	// when a caller laid out a compile it did not check. No edge beats an edge
	// from nowhere.
	RenderGraph graph;
	const ResourceId colour = graph.AddResource({.Name = Name("colour"), .Kind = ResourceKind::Colour});
	const ResourceId shadow = graph.AddResource({.Name = Name("shadow"), .Kind = ResourceKind::Depth});
	graph.AddNode({.Name = Name("opaque"), .Kind = Name("k"), .Reads = {shadow}, .Writes = {colour}});

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::ReadsBeforeWrite);

	// Laid out anyway, which is what a widget showing a broken graph does.
	compiled.PerView.push_back(engine::graph::NodeId{1});
	const PipelineLayout layout = LayoutPipeline(graph, compiled);

	CHECK(layout.Nodes.size() == 1);
	CHECK(layout.Edges.empty());
}

TEST_CASE("a disabled node is absent from the layout", "[graph]") {
	// The layout is a view of what will run. A widget wanting to grey out a
	// switched-off pass asks the graph, which still holds it.
	RenderGraph graph = StandardGraph();

	CompiledGraph before;
	Name offender;
	REQUIRE(graph.Compile(before, offender) == GraphStatus::Ok);

	engine::graph::NodeId transparent;
	for (const auto &placed : LayoutPipeline(graph, before).Nodes) {
		if (placed.Name == Name("transparent")) {
			transparent = placed.Node;
		}
	}
	REQUIRE(transparent.IsValid());
	REQUIRE(graph.SetEnabled(transparent, false));

	const PipelineLayout after = LayoutOf(graph);
	CHECK(after.Nodes.size() == 5);
	CHECK_FALSE(Joined(graph, after, "opaque", "transparent", "colour"));
}

TEST_CASE("an empty compile lays out to nothing", "[graph]") {
	const RenderGraph graph;
	const PipelineLayout layout = LayoutOf(graph);

	CHECK(layout.Nodes.empty());
	CHECK(layout.Edges.empty());
	CHECK(layout.Columns == 0);
}

TEST_CASE("every band has a description", "[graph]") {
	for (const Band band : {Band::Shared, Band::PerView, Band::Final}) {
		CHECK(std::string(Describe(band)) != "unknown");
	}
}
