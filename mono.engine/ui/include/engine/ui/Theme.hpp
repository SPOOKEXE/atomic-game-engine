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
// ten - the background behind panels, the raised face of a button, the sunken
// well of a text field, the viewport, the border - are shades of that surface.
// That is how seven themes exist without seven chances to get the contrast
// between a button and the panel it sits on subtly wrong, and it is the
// discipline `ui::Metrics` already applies to spacing: an interface built from
// five numbers looks designed, one built from thirty looks like nobody was in
// charge.
//
// The palettes are dark, low-chroma and low-contrast between neighbouring
// surfaces, which is not a taste claim - the thing being looked at is a lit 3D
// scene, and chrome that competes with it for attention makes the render harder
// to judge. That is the same reason a video editor is grey. `Terminal` is the
// deliberate exception and says so.
//
// @tier L12 · client

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::ui {

	// The colour themes an author may choose between.
	//
	// **Ordered dark-to-loud, because that is the order somebody scrolls
	// through them in.** Not alphabetical: `Dark` is the default and belongs
	// first, and the four that differ only in their tint belong together.
	//
	// @since v0.7
	enum class Palette : uint8_t {
		// The default. Deep blue-grey chrome and a bright blue accent - the
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
	// **Because imgui's style holds one palette at a time - the live one.** A
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
	// would draw one frame as a mixture of two - which is visible, and reads as
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
	// the first `NewFrame` - a handler registered after that has already had
	// its lines skipped as unknown, and the setting silently never restores.
	//
	// Idempotent, and a no-op when there is no context.
	void InstallThemeSettings();

	// One of the colours a theme is built from.
	//
	// **Seven, and they are every colour this module has.** Five come from the
	// palette and two are the semantic pair that never did - a warning and an
	// error read the same in every theme, because what they mean does not change
	// with the chrome. Overriding them is still allowed: somebody who cannot
	// distinguish the default red from the default green needs to move it, and
	// "the theme is customisable except for the two colours that matter most to
	// you" is not a customisable theme.
	//
	// **The list is short on purpose.** imgui has fifty-odd style slots and this
	// exposes seven, because the other forty-odd are *derived* - see the header
	// note. Letting somebody set a button's face independently of the panel
	// behind it is letting them put a control at the wrong distance from its
	// surface, which is the mistake the shade ladder exists to make impossible.
	// Seven knobs move everything, consistently.
	//
	// @since v0.13
	enum class ThemeColour : uint8_t {
		// The panel colour, and the anchor every surface is derived from.
		Surface,

		// The highlight, and the same colour hovered.
		Accent,
		AccentHot,

		// Ordinary text and secondary text.
		Text,
		TextMuted,

		// The semantic pair, which no palette declares.
		Warning,
		Error,
	};

	// How many there are, for a settings panel and a loop that iterates them.
	inline constexpr size_t THEME_COLOUR_COUNT = 7;

	// A colour's name, as a settings panel shows it and as a file spells it.
	//
	// **One spelling for both**, because the alternative is a label somebody
	// reads and a key nobody can guess from it - and this is the name a plugin
	// passes to `SetWidgetColour` as well.
	//
	// @param colour The colour.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ThemeColour colour);

	// The colour a name refers to.
	//
	// @param name The name, as `Describe` spells it. Case-sensitive, because a
	//        forgiving parser here is a parser that has to stay forgiving.
	// @return The colour, or nothing when the name is not one of them.
	std::optional<ThemeColour> ParseThemeColour(std::string_view name);

	// Colours chosen over the ones the palette declares.
	//
	// **Sparse, and that is the whole design.** A person who wanted a purple
	// accent chose one colour, not seven - so an override holds what was chosen
	// and nothing else, and switching palette afterwards keeps their accent and
	// moves everything else. An override that was a full copy of a palette would
	// pin all seven the moment anybody touched one, and choosing a theme would
	// stop doing anything.
	//
	// Colours are packed the way `IM_COL32` packs one, which is the order every
	// other colour in this header uses.
	//
	// @since v0.13
	struct ThemeColours {
		// What has been chosen, indexed by `ThemeColour`.
		std::optional<unsigned int> Values[THEME_COLOUR_COUNT];

		// @param colour Which one.
		// @return The chosen colour, or nothing.
		std::optional<unsigned int> &operator[](ThemeColour colour) {
			return Values[static_cast<size_t>(colour) % THEME_COLOUR_COUNT];
		}

		// @param colour Which one.
		// @return The chosen colour, or nothing.
		const std::optional<unsigned int> &operator[](ThemeColour colour) const {
			return Values[static_cast<size_t>(colour) % THEME_COLOUR_COUNT];
		}

		// Whether anything at all was chosen.
		//
		// **Checked before pushing rather than after**, so a panel with no
		// override of its own costs one comparison a frame instead of forty
		// style pushes that change nothing.
		//
		// @return True when at least one colour is set.
		bool Any() const;

		// Forgets every choice.
		void Clear();
	};

	// A colour as `RRGGBBAA` text, which is the order a person writes one in.
	//
	// **The one place the byte order is allowed to differ, and it is the text.**
	// Everything in this module carries a colour the way `IM_COL32` packs one,
	// because that is what imgui wants and a second order in the code would be a
	// bug waiting for the one call site that forgot to swap. But `IM_COL32`
	// printed as hex reads `AABBGGRR`, and a preferences file nobody can read a
	// colour out of is a preferences file nobody can fix by hand. So the swap
	// happens here, at the boundary, in two functions that are each other's
	// inverse.
	//
	// @param packed The colour, in imgui's `IM_COL32` byte order.
	// @return Eight hex digits, upper case, no prefix.
	// @since v0.13
	std::string ColourText(unsigned int packed);

	// The colour some `RRGGBBAA` text names.
	//
	// @param text Six or eight hex digits, with or without a leading `#`. Six is
	//        opaque - `#2E3440` is what somebody copies out of a palette, and
	//        making them append `FF` to it is making them get it wrong once.
	// @return The colour in `IM_COL32` byte order, or nothing when the text is
	//         not a colour.
	// @since v0.13
	std::optional<unsigned int> ParseColourText(std::string_view text);

	// A colour of the live theme - the palette, with the global override on top.
	//
	// @param colour Which one.
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int ColourOf(ThemeColour colour);

	// A colour as a palette declares it, ignoring every override.
	//
	// What a "reset this one" button puts back, and what a settings panel shows
	// as the value somebody is departing from.
	//
	// @param palette The palette.
	// @param colour  Which one.
	// @return The colour, in imgui's `IM_COL32` byte order.
	unsigned int ColourOf(Palette palette, ThemeColour colour);

	// What has been chosen over the palette, for the whole editor.
	//
	// @return The override, empty until something is set.
	const ThemeColours &GlobalColours();

	// Chooses colours over the palette, for the whole editor.
	//
	// Restyles the current context immediately and persists the choice in the
	// layout ini, for the same reasons `SetPalette` does both - see its note.
	//
	// @param colours What to choose. An unset field returns that colour to
	//        whatever the palette says, so clearing one is the same call.
	void SetGlobalColours(const ThemeColours &colours);

	// One panel's own colours, for as long as the object lives.
	//
	// **Constructed before `ImGui::Begin` and destroyed after `End`.** A window's
	// background is read at `Begin`, so an override pushed inside the window
	// colours everything in it except the window - which looks like a bug and is
	// the mistake this note exists to prevent.
	//
	// The colours are resolved the same way the global theme is: the seven are
	// the palette's, then the global override, then these, and the forty-odd
	// imgui slots are derived from the result. So a panel that sets a surface
	// gets a matching button, border and input well without naming any of them.
	//
	// **Only the slots that actually differ are pushed**, so a panel that
	// overrides one colour does not pay for the other thirty-nine.
	//
	// @since v0.13
	class ScopedColours {
	  public:
		// @param colours What this panel chose. Empty is a no-op.
		explicit ScopedColours(const ThemeColours &colours);

		~ScopedColours();

		ScopedColours(const ScopedColours &) = delete;
		ScopedColours &operator=(const ScopedColours &) = delete;

	  private:
		// How many style colours to pop. Zero when nothing was overridden, or
		// when there was no context to push into.
		int Pushed = 0;
	};

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
	// Exposed because a few things are drawn rather than styled - a selection
	// rectangle in the explorer, the run-state pill on the toolbar - and those
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

	// The colour secondary text uses - a class name beside an instance name, a
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
