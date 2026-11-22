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
		// erases rather than composites - which is why it is a separate name and
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

		// Writes `count` opaque pixels rightward from (x, y). No clipping, no
		// bounds check, no blending.
		//
		// **The caller must have clipped and marked already.** Every other entry
		// point here clips for you and records what it touched; this one does
		// neither. An x or y outside the image writes over whatever is next to
		// it in memory, and a region nobody passed to MarkRegion is drawn into
		// the image and never sent to the GPU.
		//
		// It exists for the text rasteriser, and the reason is arithmetic rather
		// than taste. A glyph is three bits wide, so a line of text is thousands
		// of runs two or three pixels long - and at that size Blend's clip,
		// dispatch and setup cost several times the handful of bytes it goes on
		// to write. Drawing a panel of text was measured at half a millisecond,
		// almost all of it spent deciding to write eight pixels.
		//
		// Inline on purpose: the whole point is that there is no call.
		//
		// @param x     Left pixel, already known to be inside the image.
		// @param y     Row, already known to be inside the image.
		// @param count Pixels to write; x + count must not exceed the width.
		// @param red   Source red channel.
		// @param green Source green channel.
		// @param blue  Source blue channel.
		void WriteOpaqueRun(int x, int y, int count, uint8_t red, uint8_t green, uint8_t blue) {
			uint8_t *pixel = Pixels.data() +
							 (static_cast<size_t>(y) * static_cast<size_t>(Width) + static_cast<size_t>(x)) *
								 BYTES_PER_PIXEL;

			for (int index = 0; index < count; index++) {
				pixel[0] = red;
				pixel[1] = green;
				pixel[2] = blue;
				pixel[3] = 255;
				pixel += BYTES_PER_PIXEL;
			}
		}

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

		// True if anything is waiting to be sent to the GPU.
		//
		// Distinct from HasContent below, and the distinction is the point of
		// keeping the image at all: a panel redrawn ten times a second and
		// presented a thousand times has content on all thousand frames and
		// something to upload on ten of them.
		bool IsDirty() const {
			return DirtyLeft < DirtyRight && DirtyTop < DirtyBottom;
		}

		// True if anything has been drawn since the last Clear that emptied the
		// image, whether or not it has been uploaded since.
		//
		// What the renderer asks before running the overlay pass. The texture
		// holds the last thing uploaded to it and goes on holding it, so a frame
		// that draws nothing new still has something to show.
		bool HasContent() const {
			return Painted;
		}

		// Declares that the GPU now matches this image.
		//
		// Called by the renderer once it has recorded the upload. Until it is,
		// UploadRegion keeps reporting the pending area - including the part a
		// shrinking panel vacated, which nothing else would remember.
		void MarkUploaded() {
			// What was pending is now what the texture is showing. The upload
			// covered the old showing region too - that is how UploadRegion is
			// built - so everything outside this rectangle is transparent on the
			// GPU as well as here.
			PreviousLeft = DirtyLeft;
			PreviousRight = DirtyRight;
			PreviousTop = DirtyTop;
			PreviousBottom = DirtyBottom;

			DirtyLeft = DirtyRight = DirtyTop = DirtyBottom = 0;
			ClearedSinceUpload = false;
		}

		// A rectangle of the image, in pixels from the top-left.
		struct Region {
			// Left edge.
			int X = 0;

			// Top edge.
			int Y = 0;

			// Width. Zero for an empty region, which is what an image nobody has
			// drawn on reports.
			int Width = 0;

			// Height. Zero for an empty region.
			int Height = 0;
		};

		// The part of the image the renderer has to send to the GPU.
		//
		// Everything drawn since the last Clear, **plus** everything drawn
		// before it. The second half is the one that is easy to miss: the GPU
		// texture keeps whatever was uploaded last time, so a panel that shrinks
		// leaves the pixels it used to occupy lit up on screen unless the area
		// it vacated is uploaded as the transparent it now is.
		//
		// This matters because the image is the size of the *window* and the
		// panels are a corner of it. Uploading all of it was, measured, the
		// largest single cost in the frame - most of it transparent pixels that
		// had not changed since the program started.
		Region UploadRegion() const;

		// Records that a region has been written to.
		//
		// Blend and Fill do this for themselves. It is public for the text
		// rasteriser, which marks a whole string once and then writes its runs
		// through WriteOpaqueRun - several thousand calls that would otherwise
		// each pay for the bookkeeping.
		//
		// Clipped to the image, so a caller may name a region that hangs over
		// the edge.
		//
		// @param x      Left edge in pixels.
		// @param y      Top edge in pixels.
		// @param width  Region width in pixels.
		// @param height Region height in pixels.
		void MarkRegion(int x, int y, int width, int height);

	  private:
		int Width = 0;
		int Height = 0;

		// Written since the last Clear, as a half-open rectangle. Empty when
		// Left is not less than Right.
		int DirtyLeft = 0;
		int DirtyRight = 0;
		int DirtyTop = 0;
		int DirtyBottom = 0;

		// The region the GPU is currently showing, as of the last upload. Kept
		// because the texture goes on holding those pixels after this image
		// stops drawing them, and nothing else will tell it they are gone.
		int PreviousLeft = 0;
		int PreviousRight = 0;
		int PreviousTop = 0;
		int PreviousBottom = 0;

		// Whether the image holds a picture, as opposed to whether that picture
		// has reached the GPU yet. See HasContent.
		bool Painted = false;

		// Whether the image has been wiped since the last upload.
		//
		// This is what separates "the texture is showing this" from "the texture
		// is wrong about this". Without it the showing region would look pending
		// forever, every frame would re-upload the same unchanged pixels, and
		// redrawing the panels less often than presenting them would save
		// nothing at all.
		bool ClearedSinceUpload = false;

		std::vector<uint8_t> Pixels;
	};
}
