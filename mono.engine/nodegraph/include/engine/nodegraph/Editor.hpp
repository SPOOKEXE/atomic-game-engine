#pragma once

// The canvas: pan, zoom, select, drag, connect, fold, and on-node widgets.
//
// **Drawn into whatever window the caller has already begun**, like any other
// ImGui widget, rather than opening one of its own. A canvas that owned its
// window would decide where it lives, which is the caller's decision in every
// program that would embed one.
//
// **Everything is painted with `ImDrawList` and hit-tested analytically against
// the same `NodeLayout` it draws from.** No ImGui widget is submitted per node,
// which is what makes zoom work: an ImGui button does not scale, so a canvas
// built from them has text that stays one size while its boxes grow.
//
// The model is untouched by anything in this file except through `Graph`'s own
// API, so an edit made here is an edit anything else could have made.

#include <engine/nodegraph/Evaluate.hpp>
#include <engine/nodegraph/Graph.hpp>
#include <engine/nodegraph/Layout.hpp>
#include <engine/nodegraph/Preview.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine::nodegraph {

	// What the canvas looks like. ImGui's 0xAABBGGRR packing.
	struct Style {
		// Behind everything.
		uint32_t Background = 0xFF0A0A0A;

		// The two grid rulings, fine inside coarse.
		//@{
		uint32_t GridFine = 0xFF1A1A1A;
		uint32_t GridCoarse = 0xFF262626;
		//@}

		// A node's box, its outline, and the outline once it is selected.
		//@{
		uint32_t NodeBody = 0xFF1E1E1E;
		uint32_t NodeBorder = 0xFF3A3A3A;
		uint32_t NodeSelected = 0xFF4CA6FF;
		//@}

		// Ordinary text, and the dimmer sort a subtitle or a readout uses.
		//@{
		uint32_t Text = 0xFFE6E6E6;
		uint32_t Muted = 0xFF8A8A8A;
		//@}

		// A knob's trough, and the part of it that is filled.
		//@{
		uint32_t Widget = 0xFF2B2B2B;
		uint32_t WidgetFill = 0xFF4A6FA5;
		//@}

		// What a link that would be refused is drawn in while it is dragged —
		// the "the drop went red" `DataTypes::CanConnect` is deliberately plain
		// enough to justify.
		uint32_t Refused = 0xFF4444EE;

		// The rubber-band rectangle. Deliberately translucent, so what is under
		// it stays readable while it is drawn.
		uint32_t Marquee = 0x224CA6FF;

		// The geometry everything is laid out from.
		Metrics Sizes;
	};

	// What a canvas asks its host to do, for the two things it cannot decide.
	//
	// **Callbacks rather than a queue of enums**, because there are two and each
	// has one caller. A canvas that owned an undo stack would be a canvas
	// deciding what a document is, and a second canvas over the same graph would
	// then own a second one.
	struct Hooks {
		// A mutation landed and the host should record it. Called once per
		// gesture, at the end — a slider drag is one undo step and not sixty.
		std::function<void()> Changed;

		// Somebody asked for one node to be computed again from scratch.
		std::function<void(NodeId)> Rerun;
	};

	// A view over one graph. Holds the camera and the drag, and nothing about
	// the model — two canvases over one graph is two views of one thing.
	class Canvas {
	  public:
		// Where thumbnails come from. Without one, a node with a preview draws
		// an empty frame rather than nothing at all, so the slot is still
		// visibly a slot.
		void Images(ImageSink sink) {
			Sink = std::move(sink);
		}

		// Draws and drives the graph inside the current ImGui window.
		void Draw(Graph &graph);

		// Shows what each node last produced. Read only, and when the graph is
		// evaluated stays the caller's decision.
		void Observe(const Evaluator *evaluator) {
			Watching = evaluator;
		}

		// What is selected, in the order it was picked.
		//
		// @return The nodes. Empty when a frame is selected instead — see
		//         `SelectedGroup`, which is the other half of the answer.
		const std::vector<NodeId> &Selection() const {
			return Chosen;
		}

		// Replaces the selection.
		//
		// **Replaces rather than adds**, because that is what a plain click
		// means; the canvas builds a multiple selection itself from a drag or a
		// modifier and hands the whole set to the second form.
		//
		// @param node The node to select alone.
		void Select(NodeId node);

		// Replaces the selection with a set.
		//
		// @param nodes The nodes, in the order they should be remembered.
		void Select(std::vector<NodeId> nodes);

		// Selects every node at the current depth.
		//
		// **The current depth and not the document**, so Select All inside a
		// compressed node takes what is in it rather than everything.
		//
		// @param graph The graph being shown.
		void SelectAll(const Graph &graph);

		// The frame the selection is, when it is a frame rather than nodes.
		GroupId SelectedGroup() const {
			return ChosenGroup;
		}

		// Frames every node, or only the selection when there is one.
		void Fit(const Graph &graph);

		// Puts one node in the middle without changing the zoom. What the Types
		// panel does when somebody clicks a row.
		void Centre(const Graph &graph, NodeId node);

		// The current scale, 1 being canvas units to screen pixels.
		//
		// @return The zoom.
		float Zoom() const {
			return Scale;
		}

		// Sets the scale, about the middle of the view.
		//
		// Clamped to a usable range: past a point the nodes are unreadable and
		// past the other the grid is a solid colour.
		//
		// @param zoom The wanted scale.
		void SetZoom(float zoom);

		// --- what the toolbar and the keys drive ------------------------------
		//
		// **Here rather than in the panel**, because each is the selection plus
		// the graph and nothing else, and the selection is the canvas's. A panel
		// re-deriving "which nodes are chosen" would be the second copy rule 2
		// is about.
		//@{
		void Delete(Graph &graph);
		void Copy(const Graph &graph);
		void Paste(Graph &graph);
		void Duplicate(Graph &graph);
		void GroupSelection(Graph &graph);
		void UngroupSelection(Graph &graph);
		void Collapse(Graph &graph, bool collapsed);
		bool CanPaste() const {
			return !Clip.Nodes().empty();
		}

		// Places a saved subtree — a library type — in the middle of the view.
		void Place(Graph &graph, const std::string &document);

		// Folds the selection into one node, and the inverse.
		void CompressSelection(Graph &graph);
		void ExpandSelection(Graph &graph);
		//@}

		// --- depth ------------------------------------------------------------

		// Which compressed node's contents are being drawn, or `NO_NODE` for the
		// root.
		//
		// **A filter over one graph and not a second graph.** Every feature —
		// framing, folding, inserting on a link — therefore works at any depth
		// with nothing here changing, because there is only ever one model.
		NodeId Inside() const {
			return Depth.empty() ? NO_NODE : Depth.back();
		}

		// The stack, outermost first. What a breadcrumb bar reads.
		const std::vector<NodeId> &Path() const {
			return Depth;
		}

		// Descends into a compressed node.
		//
		// **Changes which nodes are drawn and nothing about the graph.** Depth
		// is `Node::Owner`, so entering is a filter on the view rather than a
		// different document — which is what keeps the evaluator and the save
		// format unaware that compression exists.
		//
		// Does nothing for a node that is not compressed.
		//
		// @param graph The graph being shown.
		// @param node  The compressed node to look inside.
		void Enter(const Graph &graph, NodeId node);

		// Back out one level, or to a given depth. `Ascend(0)` is the root.
		//@{
		void Leave(const Graph &graph);
		void Ascend(const Graph &graph, size_t depth);
		//@}

		// The colours and sizes this canvas draws with.
		//
		// **Public and by value**, so a host that wants a second canvas looking
		// different changes its copy rather than a global — two node panels in
		// one editor is the case, and a shared style would make the second one
		// restyle the first.
		Style Look;

		// Where the host is told about the things a canvas may not decide.
		Hooks Signals;

		// Whether a dragged node lands on the grid. Off, because a graph is not
		// a diagram and the snap fights small adjustments.
		bool Snap = false;

		// A data type id to pick out, or empty. Everything carrying something
		// else fades.
		//
		// **Set from outside, because the question comes from outside**: it is
		// the Types panel asking "where does a field go", and answering it by
		// dimming the rest is the one view that shows the shape of an answer
		// spread over forty wires.
		std::string Highlight;

		// What the last refused connection was. Cleared by the next one that is
		// accepted.
		std::string LastRefusal;

	  private:
		//@{
		void ToScreen(float x, float y, float &outX, float &outY) const;
		void ToGraph(float x, float y, float &outX, float &outY) const;
		//@}

		void DrawGrid(float x, float y, float width, float height) const;
		void DrawGroups(const Graph &graph) const;
		void DrawLinks(const Graph &graph, size_t hovered) const;
		void DrawNode(const Graph &graph, const Node &node, const NodeLayout &layout) const;
		void DrawWidget(const Graph &graph, const Node &node, const PlacedWidget &placed) const;

		bool HitPort(
			const Graph &graph, float graphX, float graphY, NodeId &node, std::string &port, bool &input
		) const;
		NodeId HitNode(const Graph &graph, float graphX, float graphY) const;

		// The link nearest the cursor, by sampling its curve. `Graph::Links()`'s
		// index, or `npos`.
		//
		// **Sampled rather than solved.** The exact distance to a cubic is a
		// quartic root-find; sixteen points along it is a handful of
		// multiplications and is wrong by less than the grab radius, which is
		// the only accuracy a hit test has to have.
		size_t HitLink(const Graph &graph, float graphX, float graphY) const;

		// The frame whose title bar is under the cursor. A group is grabbed by
		// its bar and not by its body, so a marquee still works inside one.
		GroupId HitGroup(const Graph &graph, float graphX, float graphY) const;

		// Where a link's insert button sits, in graph space.
		bool LinkMiddle(const Graph &graph, size_t link, float &outX, float &outY) const;

		// The frame around a group's members, in graph space.
		bool GroupBounds(
			const Graph &graph,
			const engine::nodegraph::Group &group,
			float &left,
			float &top,
			float &right,
			float &bottom
		) const;

		void HandleWidget(Graph &graph, NodeId node, const PlacedWidget &placed, float graphX);
		void Palette(Graph &graph);
		void Menu(Graph &graph);

		// Splices a node into a link: the link is removed and two take its
		// place, on the first ports of the new node that will carry the type.
		void InsertOn(Graph &graph, size_t link, const std::string &type);

		float Scale = 1.0f;
		float PanX = 0.0f;
		float PanY = 0.0f;
		float OriginX = 0.0f;
		float OriginY = 0.0f;

		// How big the canvas was when it was last drawn.
		//
		// **Remembered rather than asked for.** `Fit` and `Centre` are pressed
		// from a toolbar and from a panel row, where `GetContentRegionAvail`
		// answers about *that* window — so asking would fit the graph to the
		// width of the button strip, and calling either outside a frame at all
		// would read a null window.
		//@{
		float ViewWidth = 800.0f;
		float ViewHeight = 600.0f;
		//@}

		// Whether a node is drawn in this view: exactly those at the current
		// depth.
		bool Visible(const Node &node) const {
			return node.Owner == Inside();
		}

		std::vector<NodeId> Chosen;
		GroupId ChosenGroup = NO_GROUP;
		std::vector<NodeId> Depth;

		// Exactly one drag is in progress, so this is one enum rather than five
		// booleans — five booleans is five ways to be in two states at once.
		enum class Dragging : uint8_t { None, Nodes, Link, Marquee, Pan, Widget, Group };
		Dragging Drag = Dragging::None;

		NodeId DragNode = NO_NODE;
		std::string DragPort;
		bool DragFromInput = false;
		std::string DragWidget;
		GroupId DragGroup = NO_GROUP;

		// Whether this gesture changed the graph. Reported to `Hooks::Changed`
		// once, when the mouse comes up — a drag is one edit and not one an
		// undo step per frame.
		bool Edited = false;

		float MarqueeX = 0.0f;
		float MarqueeY = 0.0f;
		float PaletteX = 0.0f;
		float PaletteY = 0.0f;
		bool PaletteOpen = false;

		// What the palette will do with the type somebody picks. Splicing into
		// a link, wiring to the port a drag came from, or neither.
		//
		// `PaletteNeeds` is which side of a candidate has to accept
		// `PaletteType`: nothing, an input, an output, or both — which is what a
		// splice into an existing link requires.
		//@{
		size_t PaletteLink = static_cast<size_t>(-1);
		std::string PaletteType;
		enum class Needs : uint8_t { Anything, Input, Output, Both };
		Needs PaletteNeeds = Needs::Anything;
		char PaletteSearch[64] = {};

		// **Asked for on one frame and opened on the next.** The context menu
		// is where "Add node..." is pressed, and opening a popup from inside the
		// popup that is closing puts the two on one another's id stack.
		bool PaletteWanted = false;
		//@}

		// What the context menu was opened over.
		//@{
		bool MenuOpen = false;
		NodeId MenuNode = NO_NODE;
		size_t MenuLink = static_cast<size_t>(-1);
		GroupId MenuGroup = NO_GROUP;
		//@}

		// **A whole graph rather than two loose vectors**, so pasting is the
		// same `Absorb` that places a library type — the remap that pasting a
		// fold needs is not a thing to write twice.
		Graph Clip;

		const Evaluator *Watching = nullptr;
		ImageSink Sink;
	};
}
