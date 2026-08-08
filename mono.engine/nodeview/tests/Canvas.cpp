// A pipeline drawn as instances, checked without a screen.
//
// **This is the whole reason the canvas is not in `studio`.** Everything below
// is components in a store, so a build server with no display asserts that a
// six-pass frame produces six boxes in three bands — which is the half of a
// node editor that can be wrong silently.

#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineView.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/nodeview/Canvas.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

TEST_SUITE_ID("engine.nodeview.canvas")
TEST_DEPENDS("engine.graph.pipelineview")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::graph::CompiledGraph;
using engine::graph::GraphStatus;
using engine::graph::LayoutPipeline;
using engine::graph::PipelineLayout;
using engine::graph::RenderGraph;
using engine::graph::StandardGraph;
using engine::nodeview::Build;
using engine::nodeview::Canvas;

namespace {
	struct Rig {
		Store Data;
		Entity Host;

		explicit Rig(std::string_view name) : Data(name) {
			engine::gui::RegisterGuiClasses();
			Host = Data.CreateInstance(engine::gui::GuiClass("Frame"), "Panel");
		}

		PipelineLayout StandardLayout() {
			const RenderGraph graph = StandardGraph();
			CompiledGraph compiled;
			Name offender;
			REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
			return LayoutPipeline(graph, compiled);
		}
	};

	size_t ChildCount(const Store &store, Entity parent) {
		size_t count = 0;
		store.EachChild(parent, [&count](Entity) { count++; });
		return count;
	}
}

TEST_CASE("the standard frame draws as six boxes in three bands", "[nodeview]") {
	Rig rig("nodeview.standard");
	const Canvas canvas = Build(rig.Data, rig.Host, rig.StandardLayout());

	REQUIRE(rig.Data.Alive(canvas.Root));
	CHECK(canvas.Nodes.size() == 6);
	CHECK(canvas.Bands.size() == 3);

	// Every box is a live instance under the root's subtree, not a handle to
	// something that was never parented.
	for (const auto &node : canvas.Nodes) {
		INFO(std::string(node.Name.Text()));
		CHECK(rig.Data.Alive(node.Box));
		CHECK(rig.Data.Alive(node.Label));
	}
}

TEST_CASE("a node's label carries its name", "[nodeview]") {
	Rig rig("nodeview.labels");
	const Canvas canvas = Build(rig.Data, rig.Host, rig.StandardLayout());

	const auto found = std::find_if(canvas.Nodes.begin(), canvas.Nodes.end(), [](const auto &node) {
		return node.Name == Name("opaque");
	});
	REQUIRE(found != canvas.Nodes.end());

	const auto *label = rig.Data.Get<engine::gui::Label>(found->Label);
	REQUIRE(label != nullptr);
	CHECK(label->Text == "opaque");
}

TEST_CASE("bands are laid out down the canvas and nodes across it", "[nodeview]") {
	// **The arrangement is the whole of what a reader gets at a glance**: what
	// runs once before the views, what each view runs, what is drawn over it.
	Rig rig("nodeview.arrangement");
	const Canvas canvas = Build(rig.Data, rig.Host, rig.StandardLayout());

	REQUIRE(canvas.Bands.size() == 3);

	const auto topOf = [&rig](Entity frame) {
		const auto *element = rig.Data.Get<engine::gui::Element>(frame);
		REQUIRE(element != nullptr);
		return element->Position.Y.Offset;
	};

	CHECK(topOf(canvas.Bands[0]) < topOf(canvas.Bands[1]));
	CHECK(topOf(canvas.Bands[1]) < topOf(canvas.Bands[2]));

	// And within the per-view band, later passes sit to the right.
	const auto leftOf = [&rig, &canvas](std::string_view name) {
		const auto found = std::find_if(canvas.Nodes.begin(), canvas.Nodes.end(), [&](const auto &node) {
			return node.Name == Name(name);
		});
		REQUIRE(found != canvas.Nodes.end());
		return rig.Data.Get<engine::gui::Element>(found->Box)->Position.X.Offset;
	};

	CHECK(leftOf("surface") < leftOf("opaque"));
	CHECK(leftOf("opaque") < leftOf("transparent"));
}

TEST_CASE("rebuilding replaces the canvas rather than stacking a second", "[nodeview]") {
	// A panel rebuilds after every edit, so a build that appended would leak an
	// instance tree per keystroke.
	Rig rig("nodeview.rebuild");

	const Canvas first = Build(rig.Data, rig.Host, rig.StandardLayout());
	REQUIRE(ChildCount(rig.Data, rig.Host) == 1);

	const Canvas second = Build(rig.Data, rig.Host, rig.StandardLayout());
	CHECK(ChildCount(rig.Data, rig.Host) == 1);

	// The one child is the new root, not the old one kept alongside it.
	Entity only;
	rig.Data.EachChild(rig.Host, [&only](Entity child) { only = child; });
	CHECK(only == second.Root);
	CHECK(only != first.Root);
	CHECK(rig.Data.Alive(second.Root));
}

TEST_CASE("a disabled pass leaves the canvas", "[nodeview]") {
	// The layout is a view of what will run, so the canvas is too.
	Rig rig("nodeview.disabled");
	RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	engine::graph::NodeId transparent;
	for (const auto &placed : LayoutPipeline(graph, compiled).Nodes) {
		if (placed.Name == Name("transparent")) {
			transparent = placed.Node;
		}
	}
	REQUIRE(graph.SetEnabled(transparent, false));
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	const Canvas canvas = Build(rig.Data, rig.Host, LayoutPipeline(graph, compiled));
	CHECK(canvas.Nodes.size() == 5);

	const bool present = std::any_of(canvas.Nodes.begin(), canvas.Nodes.end(), [](const auto &node) {
		return node.Name == Name("transparent");
	});
	CHECK_FALSE(present);
}

TEST_CASE("an empty pipeline builds a canvas and no bands", "[nodeview]") {
	// **A canvas, empty, rather than nothing at all.** A world with no pipeline
	// should show an editor to start one in.
	Rig rig("nodeview.empty");
	const Canvas canvas = Build(rig.Data, rig.Host, PipelineLayout{});

	CHECK(rig.Data.Alive(canvas.Root));
	CHECK(canvas.Nodes.empty());
	CHECK(canvas.Bands.empty());
}

TEST_CASE("an invalid parent builds nothing rather than a loose tree", "[nodeview]") {
	Rig rig("nodeview.noparent");
	const Canvas canvas = Build(rig.Data, Entity{}, rig.StandardLayout());

	CHECK(canvas.Root == engine::ecs::NULL_ENTITY);
	CHECK(canvas.Nodes.empty());
}

// --- edges --------------------------------------------------------------------

TEST_CASE("every derived edge is drawn as a line", "[nodeview]") {
	Rig rig("nodeview.edges");
	const PipelineLayout layout = rig.StandardLayout();
	const Canvas canvas = Build(rig.Data, rig.Host, layout);

	// One line per edge the layout derived — not per resource, and not per pair
	// of boxes that happen to be adjacent.
	REQUIRE(!layout.Edges.empty());
	CHECK(canvas.Edges.size() == layout.Edges.size());

	for (const Entity line : canvas.Edges) {
		CHECK(rig.Data.Alive(line));
	}
}

TEST_CASE("a line spans the gap between the boxes it joins", "[nodeview]") {
	// **The check that says the geometry is real.** A line of the wrong length
	// still draws, and still looks like a node editor at a glance.
	Rig rig("nodeview.edge.geometry");
	const engine::nodeview::CanvasStyle style;
	const Canvas canvas = Build(rig.Data, rig.Host, rig.StandardLayout(), style);

	// `surface` to `opaque` is one column apart in the per-view band, so the
	// centres are exactly one column pitch apart and the line is that long.
	const auto *line = rig.Data.Get<engine::gui::Element>(canvas.Edges.front());
	REQUIRE(line != nullptr);
	CHECK(line->Size.X.Offset > 0.0f);
	CHECK(line->Size.Y.Offset < line->Size.X.Offset);

	// Every line is at least as long as the gap it has to cross.
	for (const Entity entity : canvas.Edges) {
		const auto *element = rig.Data.Get<engine::gui::Element>(entity);
		REQUIRE(element != nullptr);
		INFO("length " << element->Size.X.Offset);
		CHECK(element->Size.X.Offset >= style.ColumnGap);
	}
}

TEST_CASE("an edge crossing bands is rotated rather than horizontal", "[nodeview]") {
	// `shadow` is shared and `surface` is per view, so the line between them
	// runs down as well as across. A canvas that drew every edge horizontally
	// would be claiming the shadow map is sampled by nothing below it.
	Rig rig("nodeview.edge.rotation");
	const Canvas canvas = Build(rig.Data, rig.Host, rig.StandardLayout());

	bool anyRotated = false;
	for (const Entity entity : canvas.Edges) {
		const auto *element = rig.Data.Get<engine::gui::Element>(entity);
		REQUIRE(element != nullptr);
		if (element->Rotation != 0.0f) {
			anyRotated = true;
		}
	}
	CHECK(anyRotated);
}

TEST_CASE("a line carries the resource it is about", "[nodeview]") {
	// So an inspector can say *why* two passes are joined, which is the whole
	// point of `PlacedEdge::Resource` surviving into the canvas.
	Rig rig("nodeview.edge.names");
	const Canvas canvas = Build(rig.Data, rig.Host, rig.StandardLayout());

	bool named = false;
	for (const Entity entity : canvas.Edges) {
		if (rig.Data.InstanceNameOf(entity) == Name("colour")) {
			named = true;
		}
	}
	CHECK(named);
}

TEST_CASE("an empty pipeline draws no lines", "[nodeview]") {
	Rig rig("nodeview.edge.empty");
	const Canvas canvas = Build(rig.Data, rig.Host, PipelineLayout{});
	CHECK(canvas.Edges.empty());
}

// --- selection ----------------------------------------------------------------

TEST_CASE("a point inside a box picks that node", "[nodeview]") {
	// **Picked at the centre of a box `Build` placed**, not at a coordinate
	// worked out by hand — so the case fails if either function moves and the
	// other does not.
	Rig rig("nodeview.pick");
	const engine::nodeview::CanvasStyle style;
	const PipelineLayout layout = rig.StandardLayout();
	const Canvas canvas = Build(rig.Data, rig.Host, layout, style);

	for (const auto &node : canvas.Nodes) {
		const auto *element = rig.Data.Get<engine::gui::Element>(node.Box);
		REQUIRE(element != nullptr);

		const auto *band = rig.Data.Get<engine::gui::Element>(rig.Data.ParentOf(node.Box));
		REQUIRE(band != nullptr);

		const float x = band->Position.X.Offset + element->Position.X.Offset + style.NodeWidth * 0.5f;
		const float y = band->Position.Y.Offset + element->Position.Y.Offset + style.NodeHeight * 0.5f;

		INFO(std::string(node.Name.Text()));
		const engine::nodeview::Pick pick = engine::nodeview::PickAt(layout, style, x, y);
		CHECK(pick.Hit);
		CHECK(pick.Name == node.Name);
	}
}

TEST_CASE("empty space picks nothing", "[nodeview]") {
	Rig rig("nodeview.pick.miss");
	const engine::nodeview::CanvasStyle style;
	const PipelineLayout layout = rig.StandardLayout();

	// Above and left of everything, and far below it.
	CHECK_FALSE(engine::nodeview::PickAt(layout, style, 0.0f, 0.0f).Hit);
	CHECK_FALSE(engine::nodeview::PickAt(layout, style, 4000.0f, 4000.0f).Hit);

	// And in the gap between two columns, which is the miss that matters: a
	// hit-test that rounded to the nearest box would select while the pointer
	// is over nothing.
	const float gap = style.Margin + style.NodeWidth + style.ColumnGap * 0.5f;
	CHECK_FALSE(engine::nodeview::PickAt(layout, style, gap, style.Margin + 1.0f).Hit);
}

TEST_CASE("an empty layout picks nothing anywhere", "[nodeview]") {
	CHECK_FALSE(
		engine::nodeview::PickAt(PipelineLayout{}, engine::nodeview::CanvasStyle{}, 20.0f, 20.0f).Hit
	);
}
