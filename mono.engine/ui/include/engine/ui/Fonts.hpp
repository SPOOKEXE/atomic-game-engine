#pragma once

// The faces the editor draws with, and the sizes it draws them at.
//
// **imgui's default font is a 13px bitmap and it shows.** Proportional, no
// hinting, no real monospace — which is fine for a debug overlay and wrong for
// something somebody looks at all day. `mono.studio/AGENTS.md` listed "no
// monospace font" as a deferred gap with the reason being that a font is a file
// this repository did not have and a licence somebody had to choose. It has
// both now.
//
// **Four families, all libre, all vendored.** `mono.vendor/fonts/` holds the
// files and their licences, and `THIRD_PARTY_NOTICES.md` records them. They are
// variable fonts used at their default instance — which for all four is the
// regular weight, and is what a UI wants. Real weight axes need a rasteriser
// that drives them, and stb_truetype does not.
//
// **A face is asked for by role, not by name.** A widget wants "the monospace
// one" rather than "JetBrains Mono", so swapping a family is a table edit here
// and not a search. Same rule as `input`'s `BINDINGS` and `ui::Theme`'s
// palette.
//
// @tier L12 · client

#include <cstdint>
#include <imgui.h>

namespace engine::ui {

	// What a face is for.
	//
	// @since v0.7
	enum class Typeface : uint8_t {
		// The one nearly everything uses. Inter, which was drawn for screens at
		// small sizes and is what makes a dense panel readable.
		Interface,

		// Code, and anything that has to line up in columns. JetBrains Mono.
		Monospace,

		// Headings and anything that wants a little more presence. Roboto,
		// which is also the family a Roblox author's eye is trained on.
		Display,

		// Broad Unicode coverage, for text this engine did not author — an
		// instance named in a script, a path, a player's name. Noto Sans.
		//
		// **Merged into every other face rather than selected**, so a name with
		// one non-Latin character does not switch font mid-word. Asking for it
		// directly is legal and rarely what is wanted.
		Fallback,

		// Not a face. The count.
		Count,
	};

	// How big.
	//
	// **A scale rather than a number, for `ui::Metrics`' reason**: a size
	// between two of these is a mistake rather than a choice, and a panel whose
	// text is one pixel off every other panel's reads as broken rather than as
	// deliberate.
	//
	// @since v0.7
	enum class TextSize : uint8_t {
		// Secondary text: a class name beside an instance name, a status line.
		Small,

		// Everything else.
		Body,

		// A heading inside a panel.
		Large,

		// Not a size. The count.
		Count,
	};

	// Loads every vendored face into the current imgui context.
	//
	// Call once, after the context exists and before the first frame — imgui
	// builds its atlas lazily in this version, so this costs the file reads and
	// not the rasterisation.
	//
	// **A missing file is not fatal.** The staged tree may not have fonts in it
	// — a build that only made the library, a program started from somewhere
	// unexpected — and an editor that refused to open over a font is worse than
	// one that opens in imgui's default. What is lost is legibility, and the log
	// says so once.
	//
	// @param scale Multiplied into every size, so one knob scales the whole
	//              interface. See `InterfaceSettings::Scale`.
	// @return `false` when no face could be loaded and the default is in use.
	bool LoadFonts(float scale);

	// The face for a role and a size.
	//
	// @param face Which family.
	// @param size How big.
	// @return The font, or `nullptr` when nothing was loaded — which
	//         `ImGui::PushFont` reads as "keep the current one", so a caller
	//         does not have to check.
	ImFont *Font(Typeface face, TextSize size = TextSize::Body);

	// Pushes a face for a scope and pops it at the end of one.
	//
	// **Because the alternative is a missing `PopFont` in the one branch that
	// returns early**, which is a font stack that grows every frame until imgui
	// asserts — a long way from the code that caused it.
	//
	// @since v0.7
	class ScopedFont {
	  public:
		// Pushes a face, or nothing when it was not loaded.
		//
		// @param face Which family.
		// @param size How big.
		ScopedFont(Typeface face, TextSize size = TextSize::Body);

		// Pops it, if one was pushed.
		~ScopedFont();

		ScopedFont(const ScopedFont &) = delete;
		ScopedFont &operator=(const ScopedFont &) = delete;

	  private:
		bool Pushed = false;
	};
}
