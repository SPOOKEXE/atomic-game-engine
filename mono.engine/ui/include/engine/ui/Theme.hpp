#pragma once

// What the editor looks like, in one place.
//
// **A table rather than a hundred `ImGui::PushStyleColor` calls**, for the
// reason `input`'s `BINDINGS` is a table: a colour that appears at the point of
// use is a colour nobody can change without finding every copy of it, and the
// copies drift. Retheming is editing this file.
//
// The palette is dark, low-chroma and low-contrast between neighbouring
// surfaces, which is not a taste claim — the thing being looked at is a lit 3D
// scene, and chrome that competes with it for attention makes the render harder
// to judge. That is the same reason a video editor is grey.
//
// @tier L12 · client

namespace engine::ui {

	// Applies the editor palette, spacing and rounding to the current context.
	//
	// Call after `Interface::Initialise`, which creates the context this reads.
	//
	// @param scale Multiplied into every padding, spacing and rounding, so a
	//              themed interface scales with the one knob
	//              `InterfaceSettings` offers rather than with two.
	void ApplyEditorTheme(float scale = 1.0f);

	// The accent colour, as packed RGBA a widget can pass straight to imgui.
	//
	// Exposed because a few things are drawn rather than styled — a selection
	// rectangle in the explorer, the run-state pill on the toolbar — and those
	// have to be the same colour as everything the style covers.
	//
	// @return The accent, in imgui's `IM_COL32` byte order.
	unsigned int AccentColour();

	// The colour a row uses when it is selected.
	//
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int SelectionColour();

	// The colour a warning or an unsaved marker uses.
	//
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int WarningColour();

	// The colour an error uses, in the output panel and on a failed script.
	//
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int ErrorColour();

	// The colour secondary text uses — a class name beside an instance name, a
	// property's type beside its value.
	//
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int MutedColour();
}
