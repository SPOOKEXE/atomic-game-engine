#pragma once

// A CPU-side RGBA surface, and a 3x5 pixel font to draw into it.
//
// The debug panels are drawn by hand rather than through an immediate-mode UI
// library for two reasons. They have to work when the renderer is the thing
// being debugged, so they must not share its pipeline state. And they have to
// look the same on every platform and at every resolution, which a font stack
// does not give you and a bitmap font does.
//
// One texture upload per frame, only while a panel is open.
//
// @tier L12 · client

#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::render {

	// An owned CPU image used to draw renderer-independent debug overlays.
	//
	// Pixels are tightly packed, row-major, premultiplied RGBA8 with the top-left
	// pixel first. Blend() accepts straight source channels and converts them as
	// it composites into this buffer.
	//
	// @client
	class OverlayImage {
	  public:
		// Bytes in one premultiplied RGBA8 pixel.
		static constexpr int BYTES_PER_PIXEL = 4;

		// Sets the image dimensions in pixels.
		//
		// Negative dimensions become zero. A changed size allocates a zeroed,
		// clean buffer; the same size preserves both pixels and dirty state.
		//
		// @param width  Width in pixels.
		// @param height Height in pixels.
		void Resize(int width, int height);

		// Zeros every channel without changing the dimensions and marks the image clean.
		void Clear();

		// Alpha-blends a filled rectangle. Clipped to the image, so a caller
		// may draw off the edge without checking.
		//
		// Coordinates and dimensions are in pixels from the top-left. Colour
		// channels are straight RGBA8 source values; the stored RGB is
		// premultiplied by the resulting alpha. A zero alpha or empty rectangle
		// leaves the image clean.
		//
		// @param x      Left edge in pixels.
		// @param y      Top edge in pixels.
		// @param width  Rectangle width in pixels.
		// @param height Rectangle height in pixels.
		// @param red    Source red channel.
		// @param green  Source green channel.
		// @param blue   Source blue channel.
		// @param alpha  Source alpha channel.
		void
		Blend(int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);

		// Writes a rectangle without reading what is under it.
		//
		// The same bytes Blend would produce over a *transparent* destination,
		// and different bytes over any other. It exists because a large
		// translucent rectangle drawn onto a cleared image is the one case where
		// the read is provably pointless: every destination pixel is zero, so
		// every result is the same constant, and the blend is a fill wearing a
		// read-modify-write's clothing.
		//
		// That is not a micro-optimisation at the size this is used. A debug
		// panel is hundreds of pixels square, and reading it back to combine it
		// with zero was, measured, over a third of a frame.
		//
		// **Only correct on a region known to be transparent.** Anywhere else it
		// erases rather than composites — which is why it is a separate name and
		// not a flag on Blend. Clipped and marks the image dirty exactly as Blend
		// does.
		//
		// @param x      Left edge in pixels.
		// @param y      Top edge in pixels.
		// @param width  Rectangle width in pixels.
		// @param height Rectangle height in pixels.
		// @param red    Source red channel.
		// @param green  Source green channel.
		// @param blue   Source blue channel.
		// @param alpha  Source alpha channel.
		void
		Fill(int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);

		// Returns the image width in pixels.
		int GetWidth() const {
			return Width;
		}

		// Returns the image height in pixels.
		int GetHeight() const {
			return Height;
		}

		// Reports whether either image dimension is zero.
		bool IsEmpty() const {
			return Width <= 0 || Height <= 0;
		}

		// Returns the tightly packed premultiplied RGBA8 buffer, or an unspecified
		// pointer when empty.
		//
		// The pointer is borrowed from this image and remains valid until a
		// size-changing Resize or destruction. Drawing and clearing may change
		// the pointed-to bytes.
		const uint8_t *GetPixels() const {
			return Pixels.data();
		}

		// Returns the pixel buffer size in bytes.
		size_t GetByteCount() const {
			return Pixels.size();
		}

		// True if anything was drawn since the last Clear. The renderer skips
		// the upload and the pass entirely when nothing was.
		bool IsDirty() const {
			return Dirty;
		}

	  private:
		int Width = 0;
		int Height = 0;
		bool Dirty = false;
		std::vector<uint8_t> Pixels;
	};

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
		// @param text  Text to measure; unsupported characters still occupy one advance.
		// @param scale Positive integer pixel scale.
		// @return Width without a trailing inter-glyph column, or zero for empty text.
		// @client
		int Measure(std::string_view text, int scale);

		// Returns the line-to-line row height in pixels at `scale`.
		//
		// @param scale Positive integer pixel scale.
		// @return Seven pixels multiplied by `scale`.
		// @client
		int LineHeight(int scale);

		// Lowercase is drawn as uppercase — the font has one case, and
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
