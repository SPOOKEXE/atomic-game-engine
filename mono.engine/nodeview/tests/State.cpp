// Selecting and panning a node canvas, checked without a pointer.
//
// **What is left after D00041.** This file used to hold the picking too, and
// that went with the `gui` canvas it picked against — the Render Pipeline widget
// is ImGui draw lists now and asks `nodeview::HitTest`, which takes a
// free-placed `EditorGraph` rather than a layout of bands and columns. The
// Assets canvas is still a diagram and still pans, so this is the pan.

#include <engine/nodeview/State.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.nodeview.state")

using engine::nodeview::CanvasState;
using engine::nodeview::ClearSelection;
using engine::nodeview::IsSelected;
using engine::nodeview::Pan;

using Name = engine::core::Name;

TEST_CASE("panning accumulates rather than replacing", "[nodeview]") {
	CanvasState state;
	Pan(state, 10.0f, 5.0f);
	Pan(state, -4.0f, 20.0f);

	CHECK(state.PanX == 6.0f);
	CHECK(state.PanY == 25.0f);
}

TEST_CASE("an unnamed node is never reported selected", "[nodeview]") {
	// A caller loops over nodes asking about each; one with no name would
	// otherwise light up whenever nothing was chosen.
	CanvasState state;
	CHECK_FALSE(IsSelected(state, Name{}));
}
