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
		Dirty = false;
	}

	void OverlayImage::Clear() {
		std::fill(Pixels.begin(), Pixels.end(), static_cast<uint8_t>(0));
		Dirty = false;
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

		Dirty = true;

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

		Dirty = true;

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

			int cursor = x;
			for (const char character : text) {
				const uint16_t bits = Lookup(character);

				for (int row = 0; row < GLYPH_HEIGHT; row++) {
					// Runs of lit pixels, not pixels.
					//
					// A glyph row is three bits, so it is one run about as often
					// as it is three separate ones — and a call per pixel means
					// paying the clip, the bounds arithmetic and the dirty flag
					// once for every scale-by-scale block. Coalescing first turns
					// "111" from three calls into one and leaves everything else
					// alone.
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

						image.Blend(
							cursor + column * scale,
							y + row * scale,
							(end - column) * scale,
							scale,
							red,
							green,
							blue,
							255
						);

						column = end;
					}
				}

				cursor += ADVANCE * scale;
			}
		}
	}
}
