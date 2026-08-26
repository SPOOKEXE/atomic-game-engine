#pragma once

// Where a render graph's nodes sit on a canvas, and what joins them.
//
// **The half of a node editor that is arithmetic.** A Render Pipeline widget
// shaped like Blender's is wanted, and the order to build it in is settled:
// *read-only view first - columns, edges, per-node timings - then inspection,
// then editing.* This is the read-only view's data, computed with no UI in
// front of it, so the thing that decides where a node is drawn can be wrong in
// a test rather than on somebody's screen.
//
// A widget on the `gui` tree consumes this. So could a text dump, a diagnostic,
// or a Luau binding. None of them re-derives it.
//
// ## Edges are derived, because the runtime does not store any
//
// `bake::Graph` wires an output into an input, so its editor draws the wires it
// was given. `RenderGraph` deliberately has none: a node names the resources it
// reads and writes and never names another node, and the order is declaration
// order. That is the right model for a frame - see `RenderGraph.hpp` on why a
// producer-to-consumer graph over a read-modify-write chain is a cycle - but it
// means an editor has to *work out* what to draw between two boxes.
//
// **An edge is the last writer before a reader.** If `opaque` writes colour and
// `transparent` then reads it, that is one edge; if `overlay` also reads colour
// afterwards, its edge comes from `transparent` rather than from `opaque`,
// because `transparent` is what it will actually see. Drawing an edge from
// every earlier writer would fill the canvas with lines that are true about the
// resource and false about the data.
//
// @tier L9 · shared

#include <engine/graph/RenderGraph.hpp>

#include <cstdint>
#include <vector>

namespace engine::graph {

	// Which band of the frame a node is drawn in.
	//
	// **The three blocks `Compile` produces**, and they are the natural columns:
	// a reader wants to see at a glance what runs once before the views, what
	// each view runs, and what is drawn over the lot.
	//
	// @since v0.11
	enum class Band : uint8_t {
		// Runs once, before any view.
		Shared,

		// Runs once per view.
		PerView,

		// Runs once, after every view.
		Final,
	};

	// A stable, human-readable name for a band.
	//
	// @param band The band to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Band band);

	// One node, placed.
	//
	// @since v0.11
	struct PlacedNode {
		// Which node this is.
		NodeId Node;

		// Its name, so a caller needs no second lookup to draw a label.
		core::Name Name;

		// Which band it runs in.
		Band Where = Band::PerView;

		// Its position along the frame, counting from zero within its band.
		//
		// **A column and not a pixel.** How wide a node box is and how much air
		// sits between two of them is the widget's business and changes with
		// the font; which node is left of which is the graph's, and does not.
		uint32_t Column = 0;

		// How far down to stack it when several nodes share a column.
		//
		// Always zero today, because the frame is a chain - every node's column
		// is its position and no two share one. It exists because the moment a
		// graph has two independent branches this is what stops them drawing on
		// top of each other, and adding it later would change the type every
		// widget reads.
		uint32_t Row = 0;
	};

	// One line to draw between two nodes.
	//
	// @since v0.11
	struct PlacedEdge {
		// The node that wrote it, and the one that reads it.
		//@{
		NodeId From;
		NodeId To;
		//@}

		// Which resource the edge is about, so a widget can label the line and
		// an inspector can say *why* two passes are joined.
		core::Name Resource;
	};

	// A whole graph, placed.
	//
	// @since v0.11
	struct PipelineLayout {
		// Every enabled node, in execution order.
		//
		// **Disabled nodes are absent**, because `Compile` leaves them out and
		// this is a view of what will run. A widget that wants to show a
		// switched-off pass greyed out should ask the `RenderGraph`, which still
		// holds it - the distinction between a node nobody demanded and one
		// somebody disabled is one `Node::Enabled` exists to keep.
		std::vector<PlacedNode> Nodes;

		// Every edge, in the order the reading node runs.
		std::vector<PlacedEdge> Edges;

		// How many columns the widest band needs, so a caller can size a canvas
		// before walking the nodes.
		uint32_t Columns = 0;
	};

	// Works out where everything goes.
	//
	// @param graph    The graph the compile came from.
	// @param compiled What `Compile` produced.
	// @return The layout. Empty for an empty compile.
	PipelineLayout LayoutPipeline(const RenderGraph &graph, const CompiledGraph &compiled);
}
