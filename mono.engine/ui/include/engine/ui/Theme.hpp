#pragma once

// What the editor looks like, in one place.
//
// **A table rather than a hundred `ImGui::PushStyleColor` calls**, for the
// reason `input`'s `BINDINGS` is a table: a colour that appears at the point of
// use is a colour nobody can change without finding every copy of it, and the
// copies drift. Retheming is editing this file.
//
// **Every palette is generated from five colours, not from fifteen.** A theme
// declares a surface, an accent, a hot accent and two text colours; the other
// ten — the background behind panels, the raised face of a button, the sunken
// well of a text field, the viewport, the border — are shades of that surface.
// That is how seven themes exist without seven chances to get the contrast
// between a button and the panel it sits on subtly wrong, and it is the
// discipline `ui::Metrics` already applies to spacing: an interface built from
// five numbers looks designed, one built from thirty looks like nobody was in
// charge.
//
// The palettes are dark, low-chroma and low-contrast between neighbouring
// surfaces, which is not a taste claim — the thing being looked at is a lit 3D
// scene, and chrome that competes with it for attention makes the render harder
// to judge. That is the same reason a video editor is grey. `Terminal` is the
// deliberate exception and says so.
//
// @tier L12 · client

#include <cstddef>
#include <cstdint>

namespace engine::ui {

	// The colour themes an author may choose between.
	//
	// **Ordered dark-to-loud, because that is the order somebody scrolls
	// through them in.** Not alphabetical: `Dark` is the default and belongs
	// first, and the four that differ only in their tint belong together.
	//
	// @since v0.7
	enum class Palette : uint8_t {
		// The default. Deep blue-grey chrome and a bright blue accent — the
		// look this editor's reference has, and the one the greys were chosen
		// against.
		Dark,

		// Near-black, with a grey accent rather than a coloured one. For a lit
		// scene in a dark room, where a saturated accent is the brightest thing
		// on the display.
		Shadow,

		// The same structure, tinted and accented per hue. These four differ
		// from `Dark` in two constants each and in nothing else.
		Blue,
		Purple,
		Red,
		Green,

		// Black and phosphor green, text included.
		//
		// **The one palette that breaks the rule above**, deliberately: its
		// text is coloured rather than neutral, which is what makes it a poor
		// choice for judging a render and exactly what somebody choosing it is
		// asking for.
		Terminal,
	};

	// How many there are, for a settings menu that iterates them.
	inline constexpr size_t PALETTE_COUNT = 7;

	// A palette's name, as a settings panel shows it.
	//
	// @param palette The palette.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Palette palette);

	// Enough of a palette to draw a preview of it.
	//
	// **Because imgui's style holds one palette at a time — the live one.** A
	// picker that showed seven themes by reading the style would show the
	// current theme seven times, so the other six have to be readable without
	// being applied. Three colours rather than the whole spec: a surface, a
	// control on it and the accent are the relationships somebody is choosing
	// between.
	//
	// @since v0.7
	struct PaletteSample {
		// The background the sample sits on. Packed `IM_COL32`, ready for
		// `ImDrawList`.
		unsigned int Surface;

		// A control drawn on that surface, packed the same way.
		unsigned int Raised;

		// The accent the other two are chosen against, packed the same way.
		unsigned int Accent;
	};

	// What a palette looks like, without switching to it.
	//
	// @param palette The palette to sample.
	// @return Its three representative colours.
	PaletteSample SampleOf(Palette palette);

	// Which palette the colours below are currently coming from.
	//
	// @return The current palette, `Dark` until one is chosen.
	Palette CurrentPalette();

	// Switches palette and restyles the current context.
	//
	// **Applies immediately rather than on the next frame.** imgui's style is
	// read while widgets are submitted, so a palette that took effect later
	// would draw one frame as a mixture of two — which is visible, and reads as
	// a flicker rather than as a setting.
	//
	// Safe to call before a context exists: the choice is remembered and
	// applied by the next `ApplyEditorTheme`.
	//
	// @param palette The palette to use.
	void SetPalette(Palette palette);

	// Makes the chosen palette persist in the layout ini.
	//
	// **Call before the first frame**, because imgui loads its ini lazily on
	// the first `NewFrame` — a handler registered after that has already had
	// its lines skipped as unknown, and the setting silently never restores.
	//
	// Idempotent, and a no-op when there is no context.
	void InstallThemeSettings();

	// Applies the current palette, spacing and rounding to the current context.
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

	// The colour a matched run of characters takes in a filtered list.
	//
	// **A filter that only hides is half a filter.** Showing *why* a row
	// survived is what makes a fuzzy match legible rather than magic, and the
	// reference this look is based on highlights the matched characters for
	// exactly that reason.
	//
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int LinkColour();

	// The colour text takes when it is the thing being looked at.
	//
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int BrightColour();
}
