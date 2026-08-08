#pragma once

// A pipeline drawn as a node canvas, as instances rather than as pixels.
//
// **This is the Render Pipeline widget's tree and not its panel.** v0.11 §4.4
// asks for a Blender-shaped node editor and settles the order to build it in:
// read-only view first, then inspection, then editing. `graph::PipelineView`
// worked out where each node goes; this turns that into `gui` instances a
// `LayerCollector` can lay out and draw.
//
// What is deliberately *not* here is docking, menus, scrolling and input. Those
// are the studio's, and they are the part a test cannot reach — so keeping them
// out is what leaves this file assertable. `scene::OrderScene` makes the same
// argument one layer down: a renderer is the one module a test cannot exercise,
// so the counts it hands to a draw call are the last place they should be
// computed.
//
// ## Built once and rebuilt, rather than diffed
//
// `Build` destroys whatever it made last time and makes it again. A canvas is
// a few dozen instances and a pipeline changes when somebody edits it, not
// sixty times a second — and `gui::Compile` already keeps a signature so an
// unchanged tree costs nothing to draw. Diffing a node graph into an existing
// instance tree would be the expensive, subtle half of a retained UI, paid for
// a rebuild nobody will notice.
//
// @tier L10 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/graph/PipelineView.hpp>

#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::nodeview {

	// How a canvas is spaced, in pixels.
	//
	// **Numbers rather than a theme**, because there is no theme yet and
	// inventing one to hold six values would be the wrong shape to have to
	// undo. A studio that wants its own spacing passes its own.
	//
	// @since v0.11
	struct CanvasStyle {
		// How big a node box is.
		//@{
		float NodeWidth = 140.0f;
		float NodeHeight = 44.0f;
		//@}

		// The gap between two columns, and between two bands.
		//@{
		float ColumnGap = 40.0f;
		float BandGap = 28.0f;
		//@}

		// The gap between two stacked rows in one column.
		float RowGap = 12.0f;

		// How far in from the canvas edge the first node sits.
		float Margin = 16.0f;
	};

	// What `Build` made, so a caller can find its way back in.
	//
	// **Handles rather than a count**, because the next stage is inspection: a
	// panel has to turn "the pointer is over this instance" into "this node",
	// and a name is what `graph::PipelineLayout` already carries.
	//
	// @since v0.11
	struct CanvasNode {
		// Which graph node this box is.
		core::Name Name;

		// The box.
		ecs::Entity Box;

		// Its label.
		ecs::Entity Label;
	};

	// A built canvas.
	//
	// @since v0.11
	struct Canvas {
		// The frame everything else is parented to. Destroying this destroys
		// the canvas.
		ecs::Entity Root;

		// One entry per node, in the order `PipelineLayout` placed them.
		std::vector<CanvasNode> Nodes;

		// One thin frame per edge, rotated to join two boxes.
		//
		// **Lines rather than a curve**, because `gui` draws rectangles and a
		// bezier would need a mesh the widget set has no way to express. A
		// straight run between two centres is what the layout's columns were
		// arranged to make readable.
		//
		// Under `Root` and not under a band, because an edge may cross from one
		// band into the next — a shadow map written once and sampled by every
		// view is exactly that.
		std::vector<ecs::Entity> Edges;

		// One frame per band, holding that band's nodes and carrying its name.
		//
		// **Bands are containers rather than decoration.** A reader wants to see
		// at a glance what runs once before the views, what each view runs, and
		// what is drawn over the lot — and a panel that later lets somebody
		// collapse one needs something to collapse.
		std::vector<ecs::Entity> Bands;
	};

	// What a point on the canvas is over.
	//
	// @since v0.11
	struct Pick {
		// Whether anything was hit.
		bool Hit = false;

		// Which node, when `Hit`. The same name `PlacedNode` carries, so a
		// panel turns a click into a `graph::NodeId` through the layout it
		// already has rather than through a second table.
		core::Name Name;
	};

	// What is under a point, in canvas coordinates.
	//
	// **Arithmetic over the layout rather than a walk of the instances**, and
	// that is the whole reason selection is testable at all. `gui` resolves
	// rectangles during `Layout`, so hit-testing the tree would need a screen
	// size, a laid-out frame and a compile — three things a headless case does
	// not have and none of which change the answer.
	//
	// **Boxes cannot overlap**, because the layout stacks rows rather than
	// letting two nodes share a cell, so the first hit is the only hit and
	// there is no z-order to resolve.
	//
	// @param layout Where the nodes are.
	// @param style  The spacing they were built with. Passing a different one
	//               than `Build` was given picks the wrong node, which is why
	//               both take it rather than one holding it.
	// @param x      Offset from the canvas's left edge, in pixels.
	// @param y      Offset from its top edge.
	// @return What is there, or a default for empty space.
	Pick PickAt(const graph::PipelineLayout &layout, const CanvasStyle &style, float x, float y);

	// Builds a canvas under `parent`.
	//
	// **Destroys whatever it built there before.** Calling it again after an
	// edit is how a canvas is kept current; see the header note on why this is
	// a rebuild rather than a diff.
	//
	// @param store  The world the instances live in. `gui`'s classes must be
	//               registered — `gui::RegisterGuiClasses` — which `Build` does
	//               rather than requiring, because a caller that forgot would
	//               get an empty canvas and no diagnostic.
	// @param parent What to hang the canvas under. Usually a `Frame` inside a
	//               `ScreenGui`; an invalid handle builds nothing.
	// @param layout Where the nodes go, from `graph::LayoutPipeline`.
	// @param style  Spacing. Defaulted, because a caller usually has no opinion.
	// @return What was built. An empty layout builds a root and no nodes, which
	//         is what a world with no pipeline should show — a canvas, empty,
	//         rather than nothing at all.
	Canvas Build(
		ecs::Store &store,
		ecs::Entity parent,
		const graph::PipelineLayout &layout,
		const CanvasStyle &style = {}
	);
}
