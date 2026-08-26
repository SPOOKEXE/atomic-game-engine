#pragma once

// Where everything on a node is, as a pure function of what it is.
//
// **One layout, three consumers**: the painter draws these rectangles, the hit
// test compares against them, and an inspector reads the same rows. A widget
// drawn where it cannot be clicked is the failure this arrangement makes
// impossible: there is one answer to "where is that slider" and everybody asks
// it here.
//
// Graph space, with the node's origin at its top-left corner. Zoom and pan are
// the view's business and never enter this file.

#include <nodegraph/Graph.hpp>
#include <string>
#include <vector>

namespace nodegraph {

	// The sizes everything is built from, in graph space.
	struct Metrics {
		// The title bar's height.
		float HeaderHeight = 26.0f;

		// One port's row.
		float RowHeight = 20.0f;

		// One knob's row, which is taller than a port's because a knob is
		// dragged and a port is only hit.
		float WidgetHeight = 22.0f;

		// The margin inside a node's border.
		float Padding = 8.0f;

		// A port circle's radius, and therefore half its hit target.
		float PortRadius = 5.0f;

		// The corner radius of a node's box.
		float Rounding = 6.0f;

		// Point size for a title and a port name.
		float LabelSize = 13.0f;

		// Point size for a subtitle and a value readout.
		float SmallSize = 11.0f;
	};

	// A port, placed relative to its node's top-left corner.
	struct PlacedPort {
		// The port's name, as its declaration spells it.
		std::string Name;

		// Its `DataType::Id`, copied so a hit test can colour it without
		// looking the declaration up again.
		std::string Type;

		// Which side it is on.
		bool Input = false;

		// The circle's centre, relative to the node's top-left.
		//@{
		float X = 0.0f;
		float Y = 0.0f;
		//@}
	};

	// A widget, placed. The rectangle is the row it may be dragged in.
	struct PlacedWidget {
		// Which knob this is, by `WidgetSpec::Key`.
		std::string Key;

		// Its position in the declaration, so a caller that has the type in
		// hand can reach the spec without a lookup by key.
		size_t Index = 0;

		// The row's top-left, relative to the node's.
		//@{
		float X = 0.0f;
		float Y = 0.0f;
		//@}

		// The row's extent.
		//@{
		float Width = 0.0f;
		float Height = 0.0f;
		//@}
	};

	// One node's geometry, and the one answer to "where is that slider".
	struct NodeLayout {
		// The node's box, in canvas units. The width is its type's; the height
		// is whatever the rows below added up to.
		//@{
		float Width = 0.0f;
		float Height = 0.0f;
		//@}

		// Every port, placed. Inputs and outputs together. `PlacedPort::Input` is
		// what separates them, so one loop draws both.
		std::vector<PlacedPort> Ports;

		// Every knob, placed, in declaration order.
		std::vector<PlacedWidget> Widgets;

		// Where the knobs start, so a collapsed node can clip below it without
		// re-deriving the header and port block.
		float WidgetsTop = 0.0f;

		// The thumbnail's square, for a type that has a `Preview`. Zero-sided
		// when it has none, which is what a node carrying a number gets: an
		// empty slot would be a picture that never arrives.
		//@{
		float PreviewTop = 0.0f;
		float PreviewSide = 0.0f;
		//@}

		// The row a progress bar goes in while the node is working. Always
		// present on an async type, so a node does not change height when it
		// starts: a graph that reflowed as it ran would be one nobody could click
		// in.
		//@{
		float ProgressTop = 0.0f;
		float ProgressHeight = 0.0f;
		//@}
	};

	// The interface a node actually has.
	//
	// **A node and not its type**, because a compressed node's ports and knobs
	// come from how its contents were wired rather than from a declaration.
	// Everything downstream (layout, hit testing, the inspector) reads these so
	// that none of them can disagree with what the canvas drew.
	//@{
	std::vector<PortSpec> InputsOf(const Node &node);
	std::vector<PortSpec> OutputsOf(const Node &node);
	std::vector<WidgetSpec> WidgetsOf(const Node &node);
	//@}

	// Where a port's payload really is: itself, or the inner port a compressed
	// node's proxy stands for.
	//
	// **Resolved by whoever reads it rather than republished by the
	// evaluator.** A compressed node's output *is* its inner node's output;
	// copying it forward would be a second field's worth of memory every frame
	// for a node that computed nothing.
	//
	// @return False when the node or port does not exist.
	bool Actual(
		const Graph &graph,
		NodeId node,
		const std::string &port,
		bool input,
		NodeId &outNode,
		std::string &outPort
	);

	// The inverse, and the one a view asks: where does this port *appear* at a
	// given depth? The node itself when it already sits there, or the compressed
	// node standing in for it and the proxy port that does.
	//
	// **One walk, used by both the canvas and by compression itself.** A wire
	// crossing into a folded selection has to be drawn to the fold, and a
	// selection being folded has to know which of its members an outside wire
	// arrives at. Those are the same question asked from two sides, and two
	// implementations of it is how a wire ends up drawn to the wrong port.
	//
	// @return False when the port is not visible at `depth` at all.
	bool Standing(
		const Graph &graph,
		NodeId node,
		const std::string &port,
		bool input,
		NodeId depth,
		NodeId &outNode,
		std::string &outPort
	);

	// Where a knob really lives: itself, or the inner node a compressed node
	// promoted it from. A write to a promoted knob is a write to the node
	// inside, which is what makes a compressed node a view of its contents.
	bool ActualWidget(
		const Graph &graph, NodeId node, const std::string &key, NodeId &outNode, std::string &outKey
	);

	// Reading and writing a knob, following a promotion to the node it really
	// lives on. **Everything that touches a value goes through these** (the
	// painter, the hit test and the inspector), because a compressed node that
	// showed one number and wrote another would be worse than one with no knobs.
	//@{
	Value ValueOf(const Graph &graph, NodeId node, const WidgetSpec &spec);
	void SetValue(Graph &graph, NodeId node, const std::string &key, const Value &value);
	//@}

	// The type a compressed node is placed as. Registered on first use, hidden
	// from the palette: one is made by folding a selection and never by
	// picking it out of a list.
	inline constexpr const char *CUSTOM_TYPE = "custom.node";

	// Whether this type's nodes get a thumbnail slot: its own `Preview`, or a
	// preview port whose data type carries one.
	bool HasPicture(const NodeType &type);

	// Lays out one node. A node of an unregistered type still gets a body, so it
	// can be seen, moved and deleted. A collapsed one gets its header and its
	// ports and nothing else.
	NodeLayout LayoutOf(const Node &node, const Metrics &metrics = Metrics{});

	// The port by name, or nullptr.
	const PlacedPort *PortIn(const NodeLayout &layout, const std::string &name, bool input);
}
