#include <engine/render/Overlay.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace engine::render {

	void OverlayImage::Resize(int width, int height) {
		width = std::max(width, 0);
		height = std::max(height, 0);

		if (width == Width && height == Height) {
			return;
		}

		Width = width;
		Height = height;
		Pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * BYTES_PER_PIXEL, 0);

		// A new buffer, and a new texture to go with it. Nothing on the GPU
		// survives a resize, so there is no vacated region to account for.
		DirtyLeft = DirtyRight = DirtyTop = DirtyBottom = 0;
		PreviousLeft = PreviousRight = PreviousTop = PreviousBottom = 0;
		Painted = false;
		ClearedSinceUpload = false;
	}

	void OverlayImage::Clear() {
		// Clearing a clean image is nothing — the buffer is already zero — and
		// it must stay nothing, because the region below is what still has to
		// reach the GPU.
		//
		// With the panels closed this runs every frame. Zeroing regardless would
		// be a full-buffer write per frame to erase nothing, and it would forget
		// the region on the *second* such frame: the pixels the panels used to
		// occupy are still on the texture, and reopening a smaller panel would
		// leave the old one's edges on screen around it.
		if (!Painted) {
			return;
		}

		// Only what was painted, not the whole buffer.
		//
		// **Painted means the dirty rectangle *or* the last uploaded one, and
		// getting that wrong is what stopped the panels clearing.** The obvious
		// invariant — everything outside the dirty rectangle is zero — is false,
		// and `MarkUploaded` is why: it empties the dirty rectangle without
		// touching a pixel, which is correct, because at that moment the image
		// and the texture agree and neither is wrong. So on the next frame the
		// buffer still holds the whole panel while the dirty rectangle claims
		// nothing has been drawn. Clearing only what was marked cleared nothing,
		// and `UploadRegion` then dutifully re-sent the old picture over itself:
		// the flamegraph never went away when it was closed, and a statistics
		// panel redrawn smaller left the previous one's glyphs beside it.
		//
		// So the union. `Previous` holds what is on the texture, `Dirty` holds
		// what has been painted since, and every non-zero pixel is inside one of
		// them.
		//
		// The old shape zeroed the entire image. On a 4K display that is
		// thirty-three megabytes of memory traffic per frame to erase two
		// panels that cover a fraction of it — and it is why the panels
		// measured fifteen times slower at 4K than at 1080p for four times the
		// pixels. The union is still bounded by what has been drawn rather than
		// by the size of the display it is drawn on, so that saving stands.
		int left = DirtyLeft;
		int top = DirtyTop;
		int right = DirtyRight;
		int bottom = DirtyBottom;

		if (PreviousLeft < PreviousRight && PreviousTop < PreviousBottom) {
			if (left >= right || top >= bottom) {
				left = PreviousLeft;
				top = PreviousTop;
				right = PreviousRight;
				bottom = PreviousBottom;
			} else {
				left = std::min(left, PreviousLeft);
				top = std::min(top, PreviousTop);
				right = std::max(right, PreviousRight);
				bottom = std::max(bottom, PreviousBottom);
			}
		}

		const size_t stride = static_cast<size_t>(Width) * BYTES_PER_PIXEL;
		const size_t run = static_cast<size_t>(right - left) * BYTES_PER_PIXEL;

		for (int row = top; row < bottom; row++) {
			uint8_t *start = Pixels.data() + static_cast<size_t>(row) * stride +
							 static_cast<size_t>(left) * BYTES_PER_PIXEL;
			std::fill(start, start + run, static_cast<uint8_t>(0));
		}

		// The uploaded region is left exactly as it was. It records what the GPU
		// is *showing*, and clearing this image does not change that — the
		// texture goes on holding the last picture sent to it until an upload
		// covering those pixels says otherwise, which is what UploadRegion is
		// for. Anything this image had painted and not yet sent never reached
		// the texture at all, so there is nothing there to correct.
		DirtyLeft = DirtyRight = DirtyTop = DirtyBottom = 0;
		Painted = false;
		ClearedSinceUpload = true;
	}

	void OverlayImage::MarkRegion(int x, int y, int width, int height) {
		if (IsEmpty() || width <= 0 || height <= 0) {
			return;
		}

		const int left = std::max(x, 0);
		const int top = std::max(y, 0);
		const int right = std::min(x + width, Width);
		const int bottom = std::min(y + height, Height);
		if (left >= right || top >= bottom) {
			return;
		}

		if (!IsDirty()) {
			DirtyLeft = left;
			DirtyTop = top;
			DirtyRight = right;
			DirtyBottom = bottom;
			Painted = true;
			return;
		}

		Painted = true;

		DirtyLeft = std::min(DirtyLeft, left);
		DirtyTop = std::min(DirtyTop, top);
		DirtyRight = std::max(DirtyRight, right);
		DirtyBottom = std::max(DirtyBottom, bottom);
	}

	OverlayImage::Region OverlayImage::UploadRegion() const {
		const bool now = DirtyLeft < DirtyRight && DirtyTop < DirtyBottom;

		// The showing region only needs sending again once this image has been
		// wiped underneath it. Until then the texture is right, and a frame that
		// redraws nothing has nothing to say.
		const bool before =
			ClearedSinceUpload && PreviousLeft < PreviousRight && PreviousTop < PreviousBottom;

		if (!now && !before) {
			return Region{};
		}
		if (!before) {
			return Region{DirtyLeft, DirtyTop, DirtyRight - DirtyLeft, DirtyBottom - DirtyTop};
		}
		if (!now) {
			return Region{
				PreviousLeft, PreviousTop, PreviousRight - PreviousLeft, PreviousBottom - PreviousTop
			};
		}

		const int left = std::min(DirtyLeft, PreviousLeft);
		const int top = std::min(DirtyTop, PreviousTop);
		const int right = std::max(DirtyRight, PreviousRight);
		const int bottom = std::max(DirtyBottom, PreviousBottom);
		return Region{left, top, right - left, bottom - top};
	}

	void OverlayImage::Fill(
		int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha
	) {
		if (IsEmpty() || alpha == 0 || width <= 0 || height <= 0) {
			return;
		}

		const int left = std::max(x, 0);
		const int top = std::max(y, 0);
		const int right = std::min(x + width, Width);
		const int bottom = std::min(y + height, Height);
		if (left >= right || top >= bottom) {
			return;
		}

		MarkRegion(left, top, right - left, bottom - top);

		// What Blend arrives at over a transparent destination: the source
		// channels premultiplied by the source alpha, and the alpha kept.
		const uint32_t source = alpha;
		const uint8_t pattern[BYTES_PER_PIXEL] = {
			static_cast<uint8_t>((red * source + 127) / 255),
			static_cast<uint8_t>((green * source + 127) / 255),
			static_cast<uint8_t>((blue * source + 127) / 255),
			alpha,
		};

		const auto span = static_cast<size_t>(right - left) * BYTES_PER_PIXEL;
		uint8_t *first = Pixels.data() +
						 (static_cast<size_t>(top) * static_cast<size_t>(Width) + static_cast<size_t>(left)) *
							 BYTES_PER_PIXEL;

		// The first row the slow way, then every other row is a copy of it. One
		// pass over the rectangle at memcpy speed, rather than a four-byte
		// read-modify-write per pixel.
		for (size_t offset = 0; offset < span; offset += BYTES_PER_PIXEL) {
			std::memcpy(first + offset, pattern, BYTES_PER_PIXEL);
		}

		for (int row = top + 1; row < bottom; row++) {
			uint8_t *destination = Pixels.data() + (static_cast<size_t>(row) * static_cast<size_t>(Width) +
													static_cast<size_t>(left)) *
													   BYTES_PER_PIXEL;
			std::memcpy(destination, first, span);
		}
	}

	void OverlayImage::Blend(
		int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha
	) {
		if (IsEmpty() || alpha == 0 || width <= 0 || height <= 0) {
			return;
		}

		// Clip once, here, so that every caller may draw off the edge.
		const int left = std::max(x, 0);
		const int top = std::max(y, 0);
		const int right = std::min(x + width, Width);
		const int bottom = std::min(y + height, Height);
		if (left >= right || top >= bottom) {
			return;
		}

		MarkRegion(left, top, right - left, bottom - top);

		const uint32_t source = alpha;
		const uint32_t inverse = 255u - source;

		// An opaque source is a store, not a blend. Every glyph in the debug
		// panels is drawn at full alpha, and the general path below spends four
		// multiplies and four divides per pixel arriving at the source colour it
		// started with — `(c * 255 + 0 + 127) / 255` is `c` for every c in 0..255,
		// exactly, so this is the same bytes by a shorter route.
		if (alpha == 255) {
			for (int row = top; row < bottom; row++) {
				uint8_t *pixel = Pixels.data() + (static_cast<size_t>(row) * static_cast<size_t>(Width) +
												  static_cast<size_t>(left)) *
													 BYTES_PER_PIXEL;

				for (int column = left; column < right; column++) {
					pixel[0] = red;
					pixel[1] = green;
					pixel[2] = blue;
					pixel[3] = 255;
					pixel += BYTES_PER_PIXEL;
				}
			}
			return;
		}

		for (int row = top; row < bottom; row++) {
			uint8_t *pixel = Pixels.data() + (static_cast<size_t>(row) * static_cast<size_t>(Width) +
											  static_cast<size_t>(left)) *
												 BYTES_PER_PIXEL;

			for (int column = left; column < right; column++) {
				// Source-over into premultiplied storage. The source channels are
				// straight; multiplying them here converts them while blending. The
				// +127 rounds rather than truncates;
				// without it, text drawn at low alpha drifts darker every time
				// something is layered on top of it.
				pixel[0] = static_cast<uint8_t>((red * source + pixel[0] * inverse + 127) / 255);
				pixel[1] = static_cast<uint8_t>((green * source + pixel[1] * inverse + 127) / 255);
				pixel[2] = static_cast<uint8_t>((blue * source + pixel[2] * inverse + 127) / 255);
				pixel[3] = static_cast<uint8_t>(std::min(255u, source + pixel[3] * inverse / 255));
				pixel += BYTES_PER_PIXEL;
			}
		}
	}
}
