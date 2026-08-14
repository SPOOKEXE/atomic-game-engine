#pragma once

// A 3x5 bitmap font, and the three calls the debug panels draw text with.
//
// **Separate from `OverlayImage` because the dependency runs one way.** The
// image is a dirty-rect RGBA surface that knows nothing about glyphs; the font
// knows how to lay bytes into one. Nothing in `Overlay.cpp` calls this, and
// everything that calls this is somewhere else - which is the shape that says
// two concepts were sharing a file.
//
// Uppercase, digits and the punctuation the panels actually use; anything else
// draws as a blank of the same width, so a stray character misaligns nothing.
//
// @client

#include <engine/render/Overlay.hpp>

#include <cstdint>
#include <string_view>

namespace engine::render {

	// A 3x5 bitmap font. Uppercase, digits and the punctuation the panels
	// actually use; anything else draws as a blank of the same width, so a
	// stray character misaligns nothing.
	namespace DebugText {

		// Glyph width in pixels at scale one.
		inline constexpr int GLYPH_WIDTH = 3;

		// Glyph height in pixels at scale one.
		inline constexpr int GLYPH_HEIGHT = 5;

		// One blank column between glyphs.
		inline constexpr int ADVANCE = GLYPH_WIDTH + 1;

		// Width in pixels the text would occupy at `scale`.
		//
		// Values below one are measured at scale one, which is what `Draw`
		// draws them at. The three calls here agree about that or a caller
		// sizes a panel for one thing and fills it with another.
		//
		// @param text  Text to measure; unsupported characters still occupy one advance.
		// @param scale Integer pixel scale; below one is treated as one.
		// @return Width without a trailing inter-glyph column, or zero for empty text.
		// @client
		int Measure(std::string_view text, int scale);

		// Returns the line-to-line row height in pixels at `scale`.
		//
		// Values below one are measured at scale one. See `Measure`.
		//
		// @param scale Integer pixel scale; below one is treated as one.
		// @return Seven pixels multiplied by the effective scale.
		// @client
		int LineHeight(int scale);

		// Lowercase is drawn as uppercase - the font has one case, and
		// silently dropping letters would be worse than shouting.
		//
		// The origin is the glyph's top-left pixel. Unsupported characters are
		// blank but still advance; drawing is clipped by OverlayImage::Blend.
		// Values below one for `scale` are drawn at scale one.
		//
		// @param image Target CPU image.
		// @param x     Left origin in pixels.
		// @param y     Top origin in pixels.
		// @param text  Text to draw.
		// @param red   Opaque glyph red channel.
		// @param green Opaque glyph green channel.
		// @param blue  Opaque glyph blue channel.
		// @param scale Integer pixel scale.
		// @client
		void Draw(
			OverlayImage &image,
			int x,
			int y,
			std::string_view text,
			uint8_t red,
			uint8_t green,
			uint8_t blue,
			int scale
		);
	}
}
