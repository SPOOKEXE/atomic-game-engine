// Selecting and panning a node canvas, checked without a pointer.
//
// **The pan signs are what this file exists for.** Drawing adds the pan and
// picking subtracts it, and a version where those agree looks identical to one
// where they do not until somebody scrolls — at which point the canvas moves one
// way and the selection moves the other.

#include <engine/graph/PipelineView.hpp>
#include <engine/nodeview/State.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.nodeview.state")
TEST_DEPENDS("engine.graph.pipelineview")

using engine::core::Name;
using engine::graph::CompiledGraph;
using engine::graph::GraphStatus;
using engine::graph::LayoutPipeline;
using engine::graph::PipelineLayout;
using engine::graph::RenderGraph;
using engine::graph::StandardGraph;
using engine::nodeview::CanvasState;
using engine::nodeview::CanvasStyle;
using engine::nodeview::ClearSelection;
using engine::nodeview::Click;
using engine::nodeview::IsSelected;
using engine::nodeview::Pan;
using engine::nodeview::PickAt;

namespace {
	PipelineLayout Standard() {
		const RenderGraph graph = StandardGraph();
		CompiledGraph compiled;
		Name offender;
		REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
		return LayoutPipeline(graph, compiled);
	}

	// The centre of the first node, in canvas coordinates.
	std::pair<float, float> FirstCentre(const CanvasStyle &style) {
		return {style.Margin + style.NodeWidth * 0.5f, style.Margin + style.NodeHeight * 0.5f};
	}
}

TEST_CASE("clicking a node selects it", "[nodeview]") {
	CanvasState state;
	const CanvasStyle style;
	const PipelineLayout layout = Standard();
	const auto [x, y] = FirstCentre(style);

	CHECK(Click(state, layout, style, x, y));
	CHECK(state.Selected == Name("shadow"));
	CHECK(IsSelected(state, Name("shadow")));
	CHECK_FALSE(IsSelected(state, Name("opaque")));
}

TEST_CASE("clicking the same node twice reports no change", "[nodeview]") {
	// A panel repaints on change; one that reported a change every click would
	// rebuild the canvas on every click.
	CanvasState state;
	const CanvasStyle style;
	const PipelineLayout layout = Standard();
	const auto [x, y] = FirstCentre(style);

	REQUIRE(Click(state, layout, style, x, y));
	CHECK_FALSE(Click(state, layout, style, x, y));
}

TEST_CASE("clicking empty space clears the selection", "[nodeview]") {
	// Click away to dismiss, which is what every canvas in every tool does. An
	// editor that kept it would leave somebody unable to deselect.
	CanvasState state;
	const CanvasStyle style;
	const PipelineLayout layout = Standard();
	const auto [x, y] = FirstCentre(style);

	REQUIRE(Click(state, layout, style, x, y));
	CHECK(Click(state, layout, style, 5000.0f, 5000.0f));
	CHECK_FALSE(state.Selected.IsValid());
}

TEST_CASE("panning moves what a point picks, by the same amount and sign", "[nodeview]") {
	// **The whole of the pan contract.** After dragging the content right by
	// one box width, the point that used to hit the first node must miss, and
	// the point one box width further right must hit it.
	CanvasState state;
	const CanvasStyle style;
	const PipelineLayout layout = Standard();
	const auto [x, y] = FirstCentre(style);

	Pan(state, style.NodeWidth + style.ColumnGap, 0.0f);

	// The old point now lands where the node no longer is.
	CanvasState probe = state;
	Click(probe, layout, style, x, y);
	CHECK(probe.Selected != Name("shadow"));

	// And the new point finds it.
	CanvasState moved = state;
	REQUIRE(Click(moved, layout, style, x + style.NodeWidth + style.ColumnGap, y));
	CHECK(moved.Selected == Name("shadow"));
}

TEST_CASE("panning accumulates rather than replacing", "[nodeview]") {
	CanvasState state;
	Pan(state, 10.0f, 5.0f);
	Pan(state, -4.0f, 20.0f);

	CHECK(state.PanX == 6.0f);
	CHECK(state.PanY == 25.0f);
}

TEST_CASE("selection survives a rebuild because it is a name", "[nodeview]") {
	// **The reason state is not held in the instances.** `Build` destroys the
	// tree and makes it again on every edit; a selection keyed by entity would
	// be lost every time somebody changed anything.
	CanvasState state;
	const CanvasStyle style;
	const auto [x, y] = FirstCentre(style);

	REQUIRE(Click(state, Standard(), style, x, y));

	// A fresh layout, as a rebuild produces. The name still resolves.
	const PipelineLayout rebuilt = Standard();
	CHECK(IsSelected(state, rebuilt.Nodes.front().Name));
}

TEST_CASE("clearing reports whether anything was selected", "[nodeview]") {
	CanvasState state;
	CHECK_FALSE(ClearSelection(state));

	const CanvasStyle style;
	const auto [x, y] = FirstCentre(style);
	REQUIRE(Click(state, Standard(), style, x, y));

	CHECK(ClearSelection(state));
	CHECK_FALSE(state.Selected.IsValid());
}

TEST_CASE("an unnamed node is never reported selected", "[nodeview]") {
	// A caller loops over nodes asking about each; one with no name would
	// otherwise light up whenever nothing was chosen.
	CanvasState state;
	CHECK_FALSE(IsSelected(state, Name{}));
}
