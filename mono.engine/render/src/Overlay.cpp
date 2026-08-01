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
		if (!IsDirty()) {
			return;
		}

		std::fill(Pixels.begin(), Pixels.end(), static_cast<uint8_t>(0));

		// This frame's region becomes last frame's. The GPU still holds those
		// pixels, so they have to be uploaded again — as transparent — or
		// whatever was drawn there stays on screen after it stops being drawn.
		PreviousLeft = DirtyLeft;
		PreviousRight = DirtyRight;
		PreviousTop = DirtyTop;
		PreviousBottom = DirtyBottom;

		DirtyLeft = DirtyRight = DirtyTop = DirtyBottom = 0;
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
			return;
		}

		DirtyLeft = std::min(DirtyLeft, left);
		DirtyTop = std::min(DirtyTop, top);
		DirtyRight = std::max(DirtyRight, right);
		DirtyBottom = std::max(DirtyBottom, bottom);
	}

	OverlayImage::Region OverlayImage::UploadRegion() const {
		const bool now = DirtyLeft < DirtyRight && DirtyTop < DirtyBottom;
		const bool before = PreviousLeft < PreviousRight && PreviousTop < PreviousBottom;

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

	namespace DebugText {

		namespace {

			// Five rows of three bits, top row first. Bit index is
			// row * 3 + column, with column 0 on the left.
			constexpr uint16_t Glyph(uint16_t r0, uint16_t r1, uint16_t r2, uint16_t r3, uint16_t r4) {
				return static_cast<uint16_t>((r0 << 0) | (r1 << 3) | (r2 << 6) | (r3 << 9) | (r4 << 12));
			}

			// Written in binary so that the shape is visible in the source.
			// Editing a glyph should not require decoding a hex constant.
			struct GlyphEntry {
				char Character;
				uint16_t Bits;
			};

			constexpr GlyphEntry GLYPHS[] = {
				{'0', Glyph(0b111, 0b101, 0b101, 0b101, 0b111)},
				{'1', Glyph(0b010, 0b110, 0b010, 0b010, 0b111)},
				{'2', Glyph(0b111, 0b001, 0b111, 0b100, 0b111)},
				{'3', Glyph(0b111, 0b001, 0b111, 0b001, 0b111)},
				{'4', Glyph(0b101, 0b101, 0b111, 0b001, 0b001)},
				{'5', Glyph(0b111, 0b100, 0b111, 0b001, 0b111)},
				{'6', Glyph(0b111, 0b100, 0b111, 0b101, 0b111)},
				{'7', Glyph(0b111, 0b001, 0b001, 0b001, 0b001)},
				{'8', Glyph(0b111, 0b101, 0b111, 0b101, 0b111)},
				{'9', Glyph(0b111, 0b101, 0b111, 0b001, 0b111)},

				{'A', Glyph(0b111, 0b101, 0b111, 0b101, 0b101)},
				{'B', Glyph(0b110, 0b101, 0b110, 0b101, 0b110)},
				{'C', Glyph(0b111, 0b100, 0b100, 0b100, 0b111)},
				{'D', Glyph(0b110, 0b101, 0b101, 0b101, 0b110)},
				{'E', Glyph(0b111, 0b100, 0b111, 0b100, 0b111)},
				{'F', Glyph(0b111, 0b100, 0b111, 0b100, 0b100)},
				{'G', Glyph(0b111, 0b100, 0b101, 0b101, 0b111)},
				{'H', Glyph(0b101, 0b101, 0b111, 0b101, 0b101)},
				{'I', Glyph(0b111, 0b010, 0b010, 0b010, 0b111)},
				{'J', Glyph(0b001, 0b001, 0b001, 0b101, 0b111)},
				{'K', Glyph(0b101, 0b101, 0b110, 0b101, 0b101)},
				{'L', Glyph(0b100, 0b100, 0b100, 0b100, 0b111)},
				{'M', Glyph(0b101, 0b111, 0b111, 0b101, 0b101)},
				{'N', Glyph(0b110, 0b101, 0b101, 0b101, 0b101)},
				{'O', Glyph(0b111, 0b101, 0b101, 0b101, 0b111)},
				{'P', Glyph(0b111, 0b101, 0b111, 0b100, 0b100)},
				{'Q', Glyph(0b111, 0b101, 0b101, 0b111, 0b001)},
				{'R', Glyph(0b111, 0b101, 0b111, 0b110, 0b101)},
				{'S', Glyph(0b111, 0b100, 0b111, 0b001, 0b111)},
				{'T', Glyph(0b111, 0b010, 0b010, 0b010, 0b010)},
				{'U', Glyph(0b101, 0b101, 0b101, 0b101, 0b111)},
				{'V', Glyph(0b101, 0b101, 0b101, 0b101, 0b010)},
				{'W', Glyph(0b101, 0b101, 0b111, 0b111, 0b101)},
				{'X', Glyph(0b101, 0b101, 0b010, 0b101, 0b101)},
				{'Y', Glyph(0b101, 0b101, 0b010, 0b010, 0b010)},
				{'Z', Glyph(0b111, 0b001, 0b010, 0b100, 0b111)},

				{'.', Glyph(0b000, 0b000, 0b000, 0b000, 0b010)},
				{',', Glyph(0b000, 0b000, 0b000, 0b010, 0b100)},
				{':', Glyph(0b000, 0b010, 0b000, 0b010, 0b000)},
				{'-', Glyph(0b000, 0b000, 0b111, 0b000, 0b000)},
				{'+', Glyph(0b000, 0b010, 0b111, 0b010, 0b000)},
				{'=', Glyph(0b000, 0b111, 0b000, 0b111, 0b000)},
				{'/', Glyph(0b001, 0b001, 0b010, 0b100, 0b100)},
				{'%', Glyph(0b101, 0b001, 0b010, 0b100, 0b101)},
				{'(', Glyph(0b001, 0b010, 0b010, 0b010, 0b001)},
				{')', Glyph(0b100, 0b010, 0b010, 0b010, 0b100)},
				{'[', Glyph(0b011, 0b010, 0b010, 0b010, 0b011)},
				{']', Glyph(0b110, 0b010, 0b010, 0b010, 0b110)},
				{'<', Glyph(0b001, 0b010, 0b100, 0b010, 0b001)},
				{'>', Glyph(0b100, 0b010, 0b001, 0b010, 0b100)},
				{'!', Glyph(0b010, 0b010, 0b010, 0b000, 0b010)},
				{'?', Glyph(0b111, 0b001, 0b011, 0b000, 0b010)},
				{'*', Glyph(0b101, 0b010, 0b111, 0b010, 0b101)},
				{'#', Glyph(0b101, 0b111, 0b101, 0b111, 0b101)},
				{'_', Glyph(0b000, 0b000, 0b000, 0b000, 0b111)},
				{'|', Glyph(0b010, 0b010, 0b010, 0b010, 0b010)},
			};

			uint16_t Lookup(char character) {
				const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
				for (const auto &entry : GLYPHS) {
					if (entry.Character == upper) {
						return entry.Bits;
					}
				}
				// Space, and anything the font does not have. Advancing by the
				// same width keeps a stray character from shifting a column of
				// numbers.
				return 0;
			}
		}

		int Measure(std::string_view text, int scale) {
			if (text.empty()) {
				return 0;
			}
			// The last glyph does not need its trailing blank column.
			return (static_cast<int>(text.size()) * ADVANCE - 1) * scale;
		}

		int LineHeight(int scale) {
			return (GLYPH_HEIGHT + 2) * scale;
		}

		void Draw(
			OverlayImage &image,
			int x,
			int y,
			std::string_view text,
			uint8_t red,
			uint8_t green,
			uint8_t blue,
			int scale
		) {
			scale = std::max(scale, 1);

			if (image.IsEmpty() || text.empty()) {
				return;
			}

			// Clipped once for the whole string rather than once per run.
			//
			// A line of forty-odd characters is five glyph rows of up to two
			// runs each — hundreds of runs two or three pixels wide. Deciding
			// separately for each one whether it is on screen costs more than
			// drawing it, and the answer is the same for all of them almost
			// every time: a debug panel is drawn inside the window it is
			// measuring.
			const int right = x + static_cast<int>(text.size()) * ADVANCE * scale;
			const int bottom = y + GLYPH_HEIGHT * scale;
			const bool inside = x >= 0 && y >= 0 && right <= image.GetWidth() && bottom <= image.GetHeight();

			// Once for the whole string. WriteOpaqueRun below records nothing:
			// it is called thousands of times a frame, and the bookkeeping would
			// cost more than the pixels it writes.
			image.MarkRegion(x, y, right - x, bottom - y);

			int cursor = x;
			for (const char character : text) {
				const uint16_t bits = Lookup(character);

				// A space, and everything else with nothing lit in it. Padding
				// to a fixed column width means a row is mostly these, and
				// walking fifteen bits to conclude so is fifteen bits wasted.
				if (bits == 0) {
					cursor += ADVANCE * scale;
					continue;
				}

				for (int row = 0; row < GLYPH_HEIGHT; row++) {
					// Runs of lit pixels, not pixels. A glyph row is three bits,
					// so it is one run about as often as it is two separate
					// ones, and each run is a single write instead of three.
					int column = 0;
					while (column < GLYPH_WIDTH) {
						// Column 0 is the leftmost, so it is the high bit of
						// the three.
						const auto lit = [bits, row](int at) {
							const int bit = row * GLYPH_WIDTH + (GLYPH_WIDTH - 1 - at);
							return (bits & (1u << bit)) != 0;
						};

						if (!lit(column)) {
							column++;
							continue;
						}

						int end = column + 1;
						while (end < GLYPH_WIDTH && lit(end)) {
							end++;
						}

						const int left = cursor + column * scale;
						const int top = y + row * scale;
						const int span = (end - column) * scale;

						if (inside) {
							for (int line = 0; line < scale; line++) {
								image.WriteOpaqueRun(left, top + line, span, red, green, blue);
							}
						} else {
							// Off the edge, or an image too small to hold the
							// line. Rare, and it goes the long way round rather
							// than repeating the clip arithmetic above.
							image.Blend(left, top, span, scale, red, green, blue, 255);
						}

						column = end;
					}
				}

				cursor += ADVANCE * scale;
			}
		}
	}
}
