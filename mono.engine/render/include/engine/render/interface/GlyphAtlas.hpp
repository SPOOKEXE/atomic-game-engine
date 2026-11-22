#pragma once

// arch-waiver public-header: forward renderer API. Text-rendering hosts share
// this complete glyph-atlas contract.

// The four faces, rasterised once, in one place.
//
// **This is the decision the roadmap named and left open**: "the atlas is the
// decision inside it, unchanged: whichever module ends up owning the four
// faces, only one does". This module owns them.
//
// `ui` owned them first, through imgui's atlas - and that was right while the
// editor was the only thing drawing text. It stops being right the moment a
// shipped client has to draw a `ScreenGui`: `mono.client` does not link
// `Engine::ui` and must not, so an atlas that lived there would mean a second
// rasteriser over the same four files, and two answers to what a glyph looks
// like. `ui` depends on this module already, so the edge runs the way it
// needs to.
//
// **Rasterised here rather than by imgui**, using the `stb_truetype` and
// `stb_rectpack` headers `mono.vendor/imgui` already carries. Those are
// standalone public-domain headers that imgui bundles rather than parts of
// imgui: including them costs no imgui symbols and puts nothing of the editor's
// toolkit into a game binary. That is what makes one atlas possible without
// giving the client an editor.
//
// **Coverage, not colour.** A glyph is one byte of alpha per pixel; the colour
// is the draw command's. That is what lets one atlas serve every colour of text
// in a frame, and it is why the pixels below are `uint8_t` rather than a
// format.
//
// @tier L12 · client

#include <engine/core/types/Vector2.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::render {

	// What a face is for.
	//
	// **The owner of this enum is the owner of the faces**, which is the whole
	// point of the paragraph above. `ui::Typeface` is an alias of this one
	// rather than a second declaration.
	//
	// @since v0.8
	enum class Typeface : uint8_t {
		// The one nearly everything uses. Inter, drawn for screens at small
		// sizes, which is what makes a dense panel readable.
		Interface,

		// Code, and anything that has to line up in columns. JetBrains Mono.
		Monospace,

		// Headings and anything wanting more presence. Roboto.
		Display,

		// Broad Unicode coverage, for text this engine did not author - an
		// instance named in a script, a path, a player's name. Noto Sans.
		Fallback,

		// Not a face. The count.
		Count,
	};

	// One glyph's place in the atlas and how to lay it out.
	//
	// @since v0.8
	struct Glyph {
		// Where the glyph sits in the atlas, in texels.
		//@{
		uint16_t X = 0;
		uint16_t Y = 0;
		uint16_t Width = 0;
		uint16_t Height = 0;
		//@}

		// Where to put it relative to the pen, in pixels. Y grows downward, so
		// a glyph's top is usually negative - which is the one sign everybody
		// gets backwards once.
		//@{
		float OffsetX = 0.0f;
		float OffsetY = 0.0f;
		//@}

		// How far the pen moves after drawing it.
		float Advance = 0.0f;
	};

	// Every glyph of every face at one pixel size.
	//
	// **One size per atlas rather than every size in one**, because a size is
	// chosen by a panel and an atlas is uploaded once: baking three sizes into
	// one sheet would triple what a client that only draws body text pays for.
	// A caller wanting several builds several.
	//
	// @since v0.8
	class GlyphAtlas {
	  public:
		// The codepoint range baked.
		//
		// **Latin-1 and no more, deliberately.** The whole of Unicode is a
		// hundred megabytes of sheet nobody looks at; what a fallback face is
		// *for* is the rest, and paging it in on demand is the shape that
		// works - filed rather than pretended at. A codepoint outside this range
		// resolves to nothing and a caller draws the missing-glyph box, which is
		// visible on purpose for `ImageSource`'s reason.
		//@{
		static constexpr char32_t FIRST_CODEPOINT = 32;
		static constexpr char32_t LAST_CODEPOINT = 255;
		//@}

		// Builds the atlas by rasterising the vendored faces.
		//
		// **A missing file is not fatal**, for `ui::LoadFonts`' reason: the
		// staged tree may not have fonts in it, and a client that refused to
		// start over one is worse than one that draws nothing where text goes.
		// What is lost is legibility and `Ready` says so.
		//
		// @param pixelSize The em size to rasterise at.
		// @return Whether at least one face was read and baked.
		bool Build(float pixelSize);

		// Whether anything was baked.
		bool Ready() const {
			return !Pixels.empty();
		}

		// The coverage sheet, one byte per texel, row-major.
		//
		// Valid until the next `Build`. A backend uploads this as an R8 texture.
		std::vector<uint8_t> const &Coverage() const {
			return Pixels;
		}

		// How wide and tall the sheet is, in texels.
		//@{
		uint32_t Width() const {
			return SheetWidth;
		}
		uint32_t Height() const {
			return SheetHeight;
		}
		//@}

		// The em size this was baked at.
		float PixelSize() const {
			return Size;
		}

		// How far apart two baselines sit, in pixels.
		float LineHeight() const {
			return Line;
		}

		// Where the one solid-white texel sits, in texels.
		//
		// **Baked into every atlas so a filled rectangle and a glyph go through
		// one pipeline.** Without it an untextured quad would need a second
		// pipeline with no texture bound, and two pipelines is two places for
		// the blend state to be set differently - which shows as interface
		// panels that are subtly the wrong opacity and nowhere else.
		//
		// Meaningless before `Build`; `Ready` says whether there is one.
		core::Vector2 WhiteTexel() const {
			return White;
		}

		// One glyph, or null when the face or codepoint was not baked.
		//
		// @param face      Which face.
		// @param codepoint The character.
		// @return The glyph, or `nullptr`.
		const Glyph *Find(Typeface face, char32_t codepoint) const;

		// How wide a run of ASCII text is, in pixels.
		//
		// **The measurement a layout should use rather than
		// `gui::AVERAGE_ADVANCE`.** That constant is an estimate `gui` needs
		// because it is `shared` and cannot rasterise anything; this is the real
		// number, and a backend that has an atlas should use it - the estimate
		// exists so the two do not disagree about *where* an element is, not so
		// the text stays wrong.
		//
		// @param face Which face.
		// @param text The run, as bytes. Codepoints past 255 count as missing.
		// @return The advance total, in pixels.
		float Measure(Typeface face, std::string_view text) const;

	  private:
		std::vector<uint8_t> Pixels;
		uint32_t SheetWidth = 0;
		uint32_t SheetHeight = 0;
		float Size = 0.0f;
		float Line = 0.0f;
		core::Vector2 White;

		// Indexed `[face][codepoint - FIRST_CODEPOINT]`, so a lookup is
		// arithmetic rather than a hash - this runs per character per frame.
		std::vector<Glyph> Glyphs;
	};
}
