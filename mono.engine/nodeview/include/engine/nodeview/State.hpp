#pragma once

// What a node canvas remembers between frames: what is selected, and where the
// view has been dragged to.
//
// **Separate from the canvas because it outlives it.** `Build` destroys the
// instance tree and makes it again on every edit; a selection that lived in
// those instances would be lost every time somebody changed anything, which is
// the one thing an editor must not do. So the state is keyed by node *name* —
// the same name `graph::PlacedNode` carries — and survives a rebuild that
// renumbers every entity.
//
// **And it is arithmetic, which is why it is here rather than in the studio.**
// Panning is two floats, selection is a name, and hit-testing is a rectangle
// test. None of it needs a window, so all of it can be wrong in a test. What
// the studio owns is turning a mouse into calls on this.
//
// @tier L10 · shared

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineView.hpp>
#include <engine/nodeview/Canvas.hpp>

namespace engine::nodeview {

	// A canvas's interaction state.
	//
	// @since v0.11
	struct CanvasState {
		// Which node is selected, or an invalid name for none.
		core::Name Selected;

		// How far the view has been dragged, in pixels.
		//
		// **Added to a node's position when drawing and subtracted from a
		// pointer before picking**, which is the whole of panning. Signs that
		// disagree are the classic version of this bug: the canvas moves one way
		// and selection moves the other, and it only shows once somebody scrolls
		// far enough to notice.
		//@{
		float PanX = 0.0f;
		float PanY = 0.0f;
		//@}
	};

	// Handles a click at a point on the *widget*, in widget coordinates.
	//
	// **Widget coordinates and not canvas ones**, which is the distinction the
	// pan exists to make: a caller has a pointer position relative to the panel
	// and should not have to undo the scroll itself.
	//
	// Clicking empty space clears the selection rather than keeping it. An
	// editor that kept it would leave somebody unable to deselect, and "click
	// away to dismiss" is what every canvas in every tool does.
	//
	// @param state  Updated in place.
	// @param layout Where the nodes are.
	// @param style  The spacing they were built with.
	// @param x      Pointer offset from the widget's left edge.
	// @param y      Pointer offset from its top edge.
	// @return `true` when the selection changed.
	bool Click(
		CanvasState &state, const graph::PipelineLayout &layout, const CanvasStyle &style, float x, float y
	);

	// Drags the view.
	//
	// **Unbounded on purpose.** Clamping wants the canvas's extent and the
	// widget's size, and the widget's size is the studio's; a panel that wants
	// to stop the scroll at an edge has both and can clamp what it passes.
	//
	// @param state Updated in place.
	// @param dx    How far right the *content* moves.
	// @param dy    How far down it moves.
	void Pan(CanvasState &state, float dx, float dy);

	// Clears the selection.
	//
	// @param state Updated in place.
	// @return `true` when something was deselected.
	bool ClearSelection(CanvasState &state);

	// Whether a named node is the selected one.
	//
	// @param state The state.
	// @param name  The node.
	// @return `true` when it is selected.
	bool IsSelected(const CanvasState &state, core::Name name);
}
