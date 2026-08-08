// The node editor's arithmetic: where a port is, what is under the pointer,
// and which wire may be dropped where.
//
// **This is the half of a ComfyUI-shaped editor that can be wrong quietly.** A
// hit-test that picks the box left of the one under the cursor, a zoom that
// walks the canvas out from under the pointer, a type rule that lets a depth
// buffer into a colour slot — none of them look like themselves on screen, and
// none of them is reachable through an ImGui panel. `studio/tests/Projection.cpp`
// makes the same argument for the viewport.

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/nodeview/Editor.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.nodeview.editor")
TEST_DEPENDS("engine.graph.pipelinedocument")

using Catch::Approx;
using engine::core::Name;
using engine::graph::NodeCatalogue;
using engine::graph::PortDirection;
using engine::graph::PortRef;
using engine::graph::ResourceKind;
using engine::nodeview::AddNode;
using engine::nodeview::CanvasView;
using engine::nodeview::Connect;
using engine::nodeview::Disconnect;
using engine::nodeview::EditorGraph;
using engine::nodeview::EvaluateDrop;
using engine::nodeview::FromDocument;
using engine::nodeview::HeightOf;
using engine::nodeview::HitKind;
using engine::nodeview::HitTest;
using engine::nodeview::LinksOf;
using engine::nodeview::NodeStyle;
using engine::nodeview::Point;
using engine::nodeview::PortAt;
using engine::nodeview::RemoveNode;
using engine::nodeview::SearchCatalogue;
using engine::nodeview::ToDocument;
using engine::nodeview::ZoomAbout;

namespace {
	// The standard kinds, registered once per process however many cases run.
	//
	// **Not `Reset` first.** The catalogue is process-wide by design — a kind is
	// named in save files and menus — and a case that emptied it would break
	// whichever case the runner happened to schedule beside it.
	void Kinds() {
		engine::graph::RegisterStandardNodeKinds();
	}

	// An opaque pass and a tone map, wired.
	EditorGraph Wired() {
		Kinds();
		EditorGraph graph;
		(void)AddNode(graph, Name("opaque"), Point{0.0f, 0.0f});
		(void)AddNode(graph, Name("tonemap"), Point{300.0f, 0.0f});
		return graph;
	}

	PortRef Port(const char *node, PortDirection direction, uint32_t slot) {
		return PortRef{Name(node), direction, slot};
	}
}

// --- the canvas ---------------------------------------------------------------

TEST_CASE("zooming holds the point under the cursor", "[nodeview][editor]") {
	CanvasView view;
	view.Pan = {40.0f, -15.0f};
	view.Zoom = 1.0f;

	const Point cursor{220.0f, 130.0f};
	const Point before = view.ToCanvas(cursor);

	ZoomAbout(view, cursor, 1.6f);

	// The whole contract in one line. A version that scaled the pan instead
	// zooms about the canvas origin, which throws the work off screen.
	const Point after = view.ToCanvas(cursor);
	CHECK(after.X == Approx(before.X));
	CHECK(after.Y == Approx(before.Y));
	CHECK(view.Zoom == Approx(1.6f));
}

TEST_CASE("zoom clamps and still holds the cursor", "[nodeview][editor]") {
	CanvasView view;
	const Point cursor{100.0f, 100.0f};
	const Point before = view.ToCanvas(cursor);

	ZoomAbout(view, cursor, 1000.0f);

	CHECK(view.Zoom == Approx(CanvasView::MAXIMUM_ZOOM));

	// **The anchor holds even when the factor did not apply**, which is the
	// case a version that returned early on a clamp gets wrong: the wheel stops
	// zooming and starts panning.
	const Point after = view.ToCanvas(cursor);
	CHECK(after.X == Approx(before.X));
	CHECK(after.Y == Approx(before.Y));
}

// --- geometry and hit-testing ---------------------------------------------------

TEST_CASE("a node is as tall as its busiest side", "[nodeview][editor]") {
	EditorGraph graph = Wired();
	const NodeStyle style;

	// `opaque` declares two inputs and two outputs; `tonemap` one and one.
	CHECK(HeightOf(graph.Nodes[0], style) > HeightOf(graph.Nodes[1], style));
}

TEST_CASE("the pointer finds the header, the body and empty canvas", "[nodeview][editor]") {
	EditorGraph graph = Wired();
	const NodeStyle style;
	const engine::nodeview::EditorNode &opaque = graph.Nodes[0];

	const Point header{opaque.At.X + style.Width * 0.5f, opaque.At.Y + style.HeaderHeight * 0.5f};
	CHECK(HitTest(graph, style, header).What == HitKind::Header);
	CHECK(HitTest(graph, style, header).Node == opaque.Name);

	const Point body{opaque.At.X + style.Width * 0.5f, opaque.At.Y + style.HeaderHeight + 6.0f};
	CHECK(HitTest(graph, style, body).What == HitKind::Body);

	CHECK(HitTest(graph, style, Point{-500.0f, -500.0f}).What == HitKind::None);
}

TEST_CASE("a port is grabbable from outside its box", "[nodeview][editor]") {
	EditorGraph graph = Wired();
	const NodeStyle style;
	const engine::nodeview::EditorNode &opaque = graph.Nodes[0];

	const Point dot = PortAt(opaque, PortDirection::Input, 1, style);

	// **Left of the box edge, which is where a dot on the left side is.**
	// Testing boxes before ports makes this point empty canvas, and the input
	// row unreachable — the failure reads as "the editor will not let me
	// connect anything".
	const engine::nodeview::Hit outside = HitTest(graph, style, Point{dot.X - style.PortRadius, dot.Y});
	REQUIRE(outside.What == HitKind::Port);
	CHECK(outside.Port.Direction == PortDirection::Input);
	CHECK(outside.Port.Slot == 1);
	CHECK(outside.Port.Node == opaque.Name);

	// And the nearest wins where two grabs overlap.
	const Point between = PortAt(opaque, PortDirection::Input, 0, style);
	const engine::nodeview::Hit nearer =
		HitTest(graph, style, Point{between.X, between.Y + style.PortPitch * 0.4f});
	REQUIRE(nearer.What == HitKind::Port);
	CHECK(nearer.Port.Slot == 0);
}

// --- the typed drop rule --------------------------------------------------------

TEST_CASE("a wire needs one end of each sort", "[nodeview][editor]") {
	EditorGraph graph = Wired();

	CHECK_FALSE(EvaluateDrop(
					graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Output, 0)
	)
					.Allowed);
	CHECK_FALSE(
		EvaluateDrop(graph, Port("opaque", PortDirection::Input, 0), Port("tonemap", PortDirection::Input, 0))
			.Allowed
	);

	// A pass reading what it writes in the same frame is the one cycle the
	// editor can refuse while the wire is still in the air.
	CHECK_FALSE(
		EvaluateDrop(graph, Port("opaque", PortDirection::Output, 0), Port("opaque", PortDirection::Input, 0))
			.Allowed
	);
}

TEST_CASE("a rendered target may be sampled and not the other way", "[nodeview][editor]") {
	EditorGraph graph = Wired();

	// `opaque.colour` is a Colour; `tonemap.source` is a Texture. That is the
	// narrowing rule, and it is the one every shadow map and mirror needs.
	const engine::nodeview::DropVerdict sampled = EvaluateDrop(
		graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0)
	);
	CHECK(sampled.Allowed);
	CHECK(sampled.Why.empty());
	CHECK(sampled.Resource == Name("opaque.colour"));

	// And backwards: `tonemap.colour` is a Colour, `opaque.shadow` a Texture —
	// also allowed, and dragging from the input end must reach the same verdict
	// as dragging from the output end.
	const engine::nodeview::DropVerdict reversed = EvaluateDrop(
		graph, Port("opaque", PortDirection::Input, 0), Port("tonemap", PortDirection::Output, 0)
	);
	CHECK(reversed.Allowed);
	CHECK(reversed.Resource == Name("tonemap.colour"));
}

TEST_CASE("a mismatched wire is refused with a reason", "[nodeview][editor]") {
	Kinds();
	EditorGraph graph;
	(void)AddNode(graph, Name("opaque"), Point{});
	(void)AddNode(graph, Name("transparent"), Point{300.0f, 0.0f});

	// `opaque.depth` is a Depth; `transparent.colour` wants a Colour.
	const engine::nodeview::DropVerdict refused = EvaluateDrop(
		graph, Port("opaque", PortDirection::Output, 1), Port("transparent", PortDirection::Input, 0)
	);
	CHECK_FALSE(refused.Allowed);

	// **The reason is a fact about the world, not about the tool.** A panel
	// showing "cannot connect" tells nobody what to do instead.
	CHECK(refused.Why == "a depth buffer is not a colour attachment");

	// The matching pair on the same two nodes is fine, which is what makes the
	// refusal above about the types rather than about the nodes.
	CHECK(EvaluateDrop(
			  graph, Port("opaque", PortDirection::Output, 1), Port("transparent", PortDirection::Input, 1)
	)
			  .Allowed);
}

TEST_CASE("connecting replaces what an input held", "[nodeview][editor]") {
	Kinds();
	EditorGraph graph;
	(void)AddNode(graph, Name("opaque"), Point{});
	(void)AddNode(graph, Name("tonemap"), Point{300.0f, 0.0f});
	(void)AddNode(graph, Name("surface"), Point{600.0f, 0.0f});

	REQUIRE(
		Connect(graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0))
	);
	CHECK(graph.Find(Name("tonemap"))->Inputs[0] == Name("opaque.colour"));

	// A second wire into one input replaces the first: a pass samples one
	// texture, and two wires into a slot would be one that does nothing.
	REQUIRE(
		Connect(graph, Port("surface", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0))
	);
	CHECK(graph.Find(Name("tonemap"))->Inputs[0] == Name("surface.surface"));
	CHECK(LinksOf(graph).size() == 1);
}

TEST_CASE("only an input can be unwired", "[nodeview][editor]") {
	EditorGraph graph = Wired();
	REQUIRE(
		Connect(graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0))
	);

	CHECK(Disconnect(graph, Port("tonemap", PortDirection::Input, 0)));
	CHECK(LinksOf(graph).empty());

	// **Clearing an output would delete the resource a pass writes**, which is
	// not what pulling a wire off means — every other reader would lose its
	// binding silently.
	CHECK_FALSE(Disconnect(graph, Port("opaque", PortDirection::Output, 0)));
	CHECK(graph.Find(Name("opaque"))->Outputs[0].IsValid());
}

// --- adding and removing --------------------------------------------------------

TEST_CASE("a second node of one kind gets its own name and resources", "[nodeview][editor]") {
	Kinds();
	EditorGraph graph;

	CHECK(AddNode(graph, Name("opaque"), Point{}) == Name("opaque"));
	CHECK(AddNode(graph, Name("opaque"), Point{}) == Name("opaque 2"));

	// **Its own resources too**, or the second pass would write the first's
	// targets and the two would be one node drawn twice.
	CHECK(graph.Find(Name("opaque 2"))->Outputs[0] == Name("opaque 2.colour"));
	CHECK(graph.Resources.size() == 4);

	CHECK_FALSE(AddNode(graph, Name("no such kind"), Point{}).IsValid());
}

TEST_CASE("removing a node unbinds whatever read it", "[nodeview][editor]") {
	EditorGraph graph = Wired();
	REQUIRE(
		Connect(graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0))
	);

	REQUIRE(RemoveNode(graph, Name("opaque")));

	// **The unbinding is the point.** A reader left pointing at a resource
	// nothing writes compiles to a pass sampling a texture nobody filled, which
	// is a black screen with no diagnostic.
	CHECK_FALSE(graph.Find(Name("tonemap"))->Inputs[0].IsValid());
	CHECK(graph.Resources.size() == 1);
	CHECK_FALSE(RemoveNode(graph, Name("opaque")));
}

// --- the add menu ---------------------------------------------------------------

TEST_CASE("the menu finds a kind from an abbreviation", "[nodeview][editor]") {
	Kinds();

	// The case a substring search gets wrong, and the reason this is not
	// `find()`: typing "tm" should reach "tonemap".
	const std::vector<engine::nodeview::CatalogueMatch> loose = SearchCatalogue("tm");
	REQUIRE_FALSE(loose.empty());
	CHECK(loose[0].Spec->Kind == Name("tonemap"));

	// A run of adjacent letters outranks the same letters scattered.
	const std::vector<engine::nodeview::CatalogueMatch> exact = SearchCatalogue("tone");
	REQUIRE_FALSE(exact.empty());
	CHECK(exact[0].Spec->Kind == Name("tonemap"));

	CHECK(SearchCatalogue("zzzz").empty());
	CHECK(SearchCatalogue("").size() == NodeCatalogue::All().size());
}

// --- the file -------------------------------------------------------------------

TEST_CASE("a graph round trips through its document", "[nodeview][editor]") {
	Kinds();
	EditorGraph graph;
	(void)AddNode(graph, Name("opaque"), Point{40.0f, 60.0f});
	(void)AddNode(graph, Name("tonemap"), Point{380.0f, 120.0f});
	REQUIRE(
		Connect(graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0))
	);
	graph.Find(Name("tonemap"))->Enabled = false;

	const EditorGraph back = FromDocument(ToDocument(graph));

	REQUIRE(back.Nodes.size() == 2);
	CHECK(back.Nodes[0].Name == Name("opaque"));
	CHECK(back.Nodes[1].Kind == Name("tonemap"));
	CHECK_FALSE(back.Nodes[1].Enabled);
	CHECK(back.Find(Name("tonemap"))->Inputs[0] == Name("opaque.colour"));
	CHECK(LinksOf(back).size() == 1);

	// **The positions survive**, which is the field a document had no way to
	// carry before v0.11 — a saved pipeline reopened as a row of boxes in
	// declaration order however it had been arranged.
	CHECK(back.Nodes[0].At.X == Approx(40.0f));
	CHECK(back.Nodes[1].At.Y == Approx(120.0f));
}

TEST_CASE("a document with no positions opens arranged", "[nodeview][editor]") {
	Kinds();
	EditorGraph graph;
	(void)AddNode(graph, Name("opaque"), Point{});
	(void)AddNode(graph, Name("tonemap"), Point{});

	// Everything at the origin is what a script-built pipeline looks like, and
	// what every pipeline written before `move` existed looks like.
	engine::graph::PipelineDocument document = ToDocument(graph);
	engine::graph::PipelineDocument stripped;
	for (const engine::graph::Edit &edit : document.Edits()) {
		if (edit.Kind != engine::graph::EditKind::Move) {
			stripped.Record(edit);
		}
	}

	const EditorGraph opened = FromDocument(stripped);
	REQUIRE(opened.Nodes.size() == 2);

	// Spread rather than stacked: one box with everything behind it reads as
	// the editor having failed to load the file.
	CHECK(opened.Nodes[0].At.X < opened.Nodes[1].At.X);
}

TEST_CASE("what the editor saves still compiles", "[nodeview][editor]") {
	Kinds();
	EditorGraph graph;
	(void)AddNode(graph, Name("opaque"), Point{});
	(void)AddNode(graph, Name("tonemap"), Point{300.0f, 0.0f});
	REQUIRE(
		Connect(graph, Port("opaque", PortDirection::Output, 0), Port("tonemap", PortDirection::Input, 0))
	);

	// **The end of the whole chain.** A canvas that produced a document the
	// runtime refuses would be an editor for a pipeline nobody can run, and it
	// is `graph::Build` that says so rather than anything here.
	engine::graph::RenderGraph built;
	Name offender;
	CHECK(
		engine::graph::Build(ToDocument(graph), built, offender) == engine::graph::PipelineDocumentStatus::Ok
	);
	CHECK_FALSE(offender.IsValid());
}
