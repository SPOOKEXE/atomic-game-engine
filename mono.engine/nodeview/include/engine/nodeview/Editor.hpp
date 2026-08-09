#pragma once

// A pipeline being edited: where its boxes are, what is under the pointer, and
// which wire may be dropped where.
//
// **The Render Pipeline widget was a diagram and this is what makes it an
// editor.** `graph::PipelineView` arranges a compiled frame into bands and
// columns, which is the right answer for *showing* one and the wrong shape for
// working on one: there is nowhere to put a node somebody dragged, no way to
// add a node that is not already in the frame, and no notion of a slot for a
// wire to land in. All three live here.
//
// ## The model is the graph, and the document is its file
//
// `EditorGraph` is what a panel mutates. `graph::PipelineDocument` is what a
// world saves — and `ToDocument`/`FromDocument` are the two functions that
// convert. That is the way round it has to be: an editor holds a graph and
// writes a file, and a panel that edited the edit-list directly would have to
// know that appending `reads` attaches it to whichever node was declared last.
//
// **Undo is a stack of `EditorGraph`s** rather than truncation of an edit list,
// which is the trade that buys the above. A pipeline is dozens of nodes, not
// megabytes; a snapshot per action is cheaper than the bug where an operation
// records two edits and one `Undo` leaves half of it behind.
//
// ## Why all of it is here rather than in the panel
//
// Nothing in this file draws. Hit-testing, the drop rule, the zoom-about-a-
// point arithmetic and the menu's search are the parts that are *wrong* in
// small ways that look like something else — a click that selects the box left
// of the one under the cursor, a zoom that walks the canvas out from under the
// pointer — and they are exactly the parts an ImGui panel cannot be tested
// through. `studio/Projection.hpp` makes this argument for the viewport, and
// this is the same argument for the canvas.
//
// @tier L10 · shared

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::nodeview {

	// A point on the canvas or on the screen, depending on what produced it.
	//
	// **Not `core::Vector2`**, which is a world type with a unit; this is
	// pixels, and mixing the two is how a canvas ends up with a metre in it.
	//
	// @since v0.11
	struct Point {
		// Across, then down.
		//@{
		float X = 0.0f;
		float Y = 0.0f;
		//@}

		// Whether two points are the same. Exact, because these are pixels a
		// caller computed rather than measurements.
		bool operator==(const Point &other) const = default;
	};

	// How far the canvas is scrolled and how far in it is zoomed.
	//
	// @since v0.11
	struct CanvasView {
		// Where the canvas origin sits on screen, in screen pixels.
		Point Pan;

		// Screen pixels per canvas pixel.
		float Zoom = 1.0f;

		// The range a wheel may take `Zoom` to.
		//
		// **Clamped rather than free.** Past about eight the text is the only
		// thing on screen and past about an eighth a node is three pixels, and
		// both states read as the editor having broken rather than as having
		// been scrolled too far.
		//@{
		static constexpr float MINIMUM_ZOOM = 0.2f;
		static constexpr float MAXIMUM_ZOOM = 4.0f;
		//@}

		// Canvas to screen.
		Point ToScreen(Point canvas) const {
			return {canvas.X * Zoom + Pan.X, canvas.Y * Zoom + Pan.Y};
		}

		// Screen to canvas.
		Point ToCanvas(Point screen) const {
			return {(screen.X - Pan.X) / Zoom, (screen.Y - Pan.Y) / Zoom};
		}
	};

	// Zooms about a fixed screen point.
	//
	// **About the pointer and not about the origin**, which is the difference
	// between a canvas that zooms where you are looking and one that throws your
	// work off screen every time you touch the wheel. The invariant is one line:
	// whatever canvas point was under `at` before is under `at` after.
	//
	// @param view   Changed in place.
	// @param at     The screen point to hold still — usually the cursor.
	// @param factor What to multiply the zoom by. Clamped to the view's range,
	//               and a factor that would leave the range still holds `at`.
	void ZoomAbout(CanvasView &view, Point at, float factor);

	// How a node box is drawn, in canvas pixels.
	//
	// @since v0.11
	struct NodeStyle {
		// How wide every box is.
		//
		// **One width for every node, which is not what ComfyUI does and is
		// what this needs.** A box sized to its longest port name makes the
		// columns ragged and the wires cross more than they have to; a fixed
		// width makes a pipeline scan.
		float Width = 168.0f;

		// The title bar, which is the part a drag grabs.
		float HeaderHeight = 26.0f;

		// The vertical pitch of the port rows down each side.
		float PortPitch = 20.0f;

		// How far the first port row sits below the header.
		float PortTop = 14.0f;

		// The dot's radius, and how far its centre sits outside the box edge.
		//@{
		float PortRadius = 5.0f;
		float PortInset = 0.0f;
		//@}

		// The least body height, so a node with no ports is still a box.
		float MinimumBody = 22.0f;

		// How generous the port hit-test is, as a multiple of `PortRadius`.
		//
		// **Bigger than the dot, deliberately.** A five-pixel target is one
		// nobody can hit at eighty percent zoom, and the cost of being generous
		// is nil: the nearest port wins, so overlapping targets still resolve to
		// the one somebody meant.
		float PortGrab = 2.2f;
	};

	// One node on the canvas.
	//
	// @since v0.11
	struct EditorNode {
		// What this one is called. Unique within a graph.
		core::Name Name;

		// Which kind it is, into `graph::NodeCatalogue`.
		core::Name Kind;

		// Where its top-left corner is, in canvas pixels.
		Point At;

		// Whether it runs. A disabled node is still drawn — see
		// `graph::Node::Enabled` on why the two must stay distinguishable.
		bool Enabled = true;

		// What is bound into each slot, one entry per port the kind declares.
		//
		// **Sized to the kind and holding an invalid name for an empty slot**,
		// rather than a sparse map. A slot is a position on a box; a container
		// that could be missing one would make every drawing loop check.
		//@{
		std::vector<core::Name> Inputs;
		std::vector<core::Name> Outputs;

		// What this particular node was configured with.
		//
		// **A canvas that could not carry these would make the parameters
		// unreachable.** `Node::Parameters` is the difference between a kind and
		// a node — which shader a `raster` runs, which tag a filter keeps — and
		// the document format has a word for it. Without a field here the word
		// had no writer: anything typed would be dropped on the next save.
		//
		// Sorted by key on the way out, so a document written twice with the
		// same contents is byte-identical whatever order the editor added them.
		std::vector<graph::NodeParameter> Parameters;
		//@}
	};

	// Two nodes agreeing about one resource, which is what a wire is.
	//
	// **Derived and never stored.** A link is not a thing an author edits; it is
	// what it looks like when a writer's output slot and a reader's input slot
	// name the same resource. Storing them would be a second description of the
	// binding — see `Editor.hpp`'s header note about not adding a fourth.
	//
	// @since v0.11
	struct EditorLink {
		// The resource both ends name.
		core::Name Resource;

		// Where it comes from.
		//@{
		core::Name FromNode;
		uint32_t FromSlot = 0;
		//@}

		// Where it goes.
		//@{
		core::Name ToNode;
		uint32_t ToSlot = 0;
		//@}
	};

	// A whole pipeline, as a panel holds it.
	//
	// @since v0.11
	struct EditorGraph {
		// The resources this pipeline declares, in declaration order.
		std::vector<graph::ResourceDesc> Resources;

		// Its nodes, in the order they run.
		std::vector<EditorNode> Nodes;

		// Finds a node by name.
		//
		// @param name Which.
		// @return The node, or null.
		//@{
		const EditorNode *Find(core::Name name) const;
		EditorNode *Find(core::Name name);
		//@}

		// What a resource is, by name.
		//
		// @param name Which.
		// @return Its kind, or `Texture` for a name this graph does not declare
		//         — the permissive answer, because an undeclared resource is a
		//         document somebody hand-edited and refusing every connection on
		//         it would be a panel that could not be used to fix it.
		graph::ResourceKind KindOf(core::Name name) const;
	};

	// Every wire the graph currently implies.
	//
	// **In the order the reading node runs**, so a panel drawing them in order
	// puts later wires over earlier ones and the frame reads left to right.
	//
	// @param graph The pipeline.
	// @return The links. A slot bound to a resource nothing writes produces no
	//         link, which is what an unfinished pipeline looks like rather than
	//         an error.
	std::vector<EditorLink> LinksOf(const EditorGraph &graph);

	// How tall a node's box is, given how many ports its kind declares.
	//
	// @param node  The node.
	// @param style The spacing.
	// @return The height, in canvas pixels.
	float HeightOf(const EditorNode &node, const NodeStyle &style);

	// Where one port's dot sits, in canvas pixels.
	//
	// @param node      The node it belongs to.
	// @param direction Which side.
	// @param slot      Which row.
	// @param style     The spacing.
	// @return The dot's centre.
	Point
	PortAt(const EditorNode &node, graph::PortDirection direction, uint32_t slot, const NodeStyle &style);

	// What a canvas point is over.
	//
	// @since v0.11
	enum class HitKind : uint8_t {
		// Empty canvas.
		None,

		// A node's title bar — the part a drag moves.
		Header,

		// A node's body.
		Body,

		// A port's dot.
		Port,
	};

	// What is under a point.
	//
	// @since v0.11
	struct Hit {
		// What sort of thing.
		HitKind What = HitKind::None;

		// Which node, for everything but `None`.
		core::Name Node;

		// Which port, for `Port`.
		graph::PortRef Port;
	};

	// What is under a canvas point.
	//
	// **Ports first, then boxes, and the last node wins.** A port's grab radius
	// reaches outside its box, so testing boxes first would make the dots on the
	// left edge unreachable; and nodes are drawn in order, so the one drawn last
	// is the one on top.
	//
	// @param graph The pipeline.
	// @param style The spacing it is drawn with. Passing a different one than
	//              the panel drew with picks the wrong thing, which is why both
	//              take it rather than one holding it.
	// @param at    The point, in canvas pixels.
	// @return What is there.
	Hit HitTest(const EditorGraph &graph, const NodeStyle &style, Point at);

	// Whether a wire being dragged may be dropped where it is.
	//
	// @since v0.11
	struct DropVerdict {
		// Whether the drop would be taken.
		bool Allowed = false;

		// Why not, for the panel to show while the wire is still in the air.
		// Empty when allowed.
		std::string Why;

		// What the drop would bind, when allowed.
		core::Name Resource;
	};

	// Judges a wire in flight.
	//
	// **Order-free**: a drag started at an input and dropped on an output is the
	// same connection as the other way round, and an editor that only worked in
	// one direction would be one people fought.
	//
	// @param graph The pipeline.
	// @param from  The port the drag started at.
	// @param to    The port under the pointer. An invalid one is "over nothing",
	//              which is refused with no explanation — a message that follows
	//              the cursor across empty canvas is noise.
	// @return Whether it may be dropped, and why not.
	DropVerdict EvaluateDrop(const EditorGraph &graph, const graph::PortRef &from, const graph::PortRef &to);

	// Makes the connection a `DropVerdict` allowed.
	//
	// @param graph The pipeline. Changed in place.
	// @param from  One end.
	// @param to    The other.
	// @return `false` when `EvaluateDrop` would have refused it, in which case
	//         nothing is changed.
	bool Connect(EditorGraph &graph, const graph::PortRef &from, const graph::PortRef &to);

	// Empties one input slot.
	//
	// **Only an input.** Clearing an output would be deleting the resource a
	// pass writes, which is not what pulling a wire off means — every reader of
	// it would silently lose its binding too.
	//
	// @param graph The pipeline. Changed in place.
	// @param port  Which slot.
	// @return `false` for anything but a bound input slot.
	bool Disconnect(EditorGraph &graph, const graph::PortRef &port);

	// Adds a node of a catalogued kind.
	//
	// **Names it after its kind, numbering as needed**, because a node has to
	// have a name the moment it exists — it is what a wire refers to and what a
	// document records — and asking somebody to type one before they can see the
	// box is not how any editor works.
	//
	// Also declares a resource per output slot, for the same reason: a pass that
	// writes nothing has no wire to drag, so it would be added and then be
	// unusable until somebody found the resource list.
	//
	// @param graph The pipeline. Changed in place.
	// @param kind  Which kind, into `graph::NodeCatalogue`.
	// @param at    Where to put its top-left corner.
	// @return The new node's name, or an invalid name for an uncatalogued kind.
	core::Name AddNode(EditorGraph &graph, core::Name kind, Point at);

	// Removes a node and unbinds every slot that read what it wrote.
	//
	// **The unbinding is the point.** Leaving a reader bound to a resource
	// nothing writes is a pipeline that compiles to a pass sampling a texture
	// nobody filled, which is a black screen with no diagnostic — the exact
	// shape `docs/DEFERRED.md` D00032 describes for materials.
	//
	// @param graph The pipeline. Changed in place.
	// @param name  Which node.
	// @return `false` when there is no such node.
	bool RemoveNode(EditorGraph &graph, core::Name name);

	// One entry in the add menu.
	//
	// @since v0.11
	struct CatalogueMatch {
		// Which kind. Never null.
		const graph::NodeKindSpec *Spec = nullptr;

		// How well it matched, higher first. Zero for an empty query, which
		// lists everything.
		int Score = 0;
	};

	// The add menu's contents for a query.
	//
	// **`studio::FuzzyMatch`'s rule, restated here rather than shared**, because
	// that one lives in `mono.studio` and this is `shared`: typing `tm` should
	// find `tonemap`. An editor whose search only matches what you already
	// spelled correctly is one you have to know the answer to use.
	//
	// @param query What was typed. Empty lists every kind, sorted as the
	//              catalogue sorts.
	// @return The matches, best first, then by kind name so the order is stable.
	std::vector<CatalogueMatch> SearchCatalogue(std::string_view query);

	// Turns a graph into the document a world saves.
	//
	// @param graph The pipeline.
	// @return The document. `graph::Build`ing it produces the frame the canvas
	//         shows.
	graph::PipelineDocument ToDocument(const EditorGraph &graph);

	// Reads a document back into a graph.
	//
	// **Nodes the document never placed are arranged rather than stacked**, in
	// declaration order, left to right — so a pipeline written before positions
	// existed, or by a script, opens as something readable instead of as one box
	// with everything behind it.
	//
	// @param document The saved pipeline.
	// @param style    The spacing, for the arrangement above.
	// @return The graph.
	EditorGraph FromDocument(const graph::PipelineDocument &document, const NodeStyle &style = {});
}
