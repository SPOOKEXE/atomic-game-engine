// The 3x5 font's glyph table and blitter.
//
// The table is the bulk of this file and is meant to be read as a picture: each
// glyph is five rows of three bits, most significant bit on the left, so the
// literals below look like the letter they draw. That is the whole reason it is
// written in binary and the whole reason it wants a file to itself — a table
// laid out to be recognised at a glance stops being recognisable the moment it
// is wedged between two unrelated functions.

#include <engine/render/DebugText.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace engine::render {

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

		// **Clamped the same way `Draw` clamps, because the three have to agree.**
		// `Draw` has always drawn a scale below one at scale one; these two
		// multiplied it raw. So a zero scale measured every panel at zero width
		// and then drew full-size text into it — a panel with its background
		// missing and its text running off the side, from a number nothing
		// rejected. `DebugPanelData::Scale` is a public `int` and nothing bounds
		// it, so the disagreement was reachable from outside the module.
		int Measure(std::string_view text, int scale) {
			if (text.empty()) {
				return 0;
			}
			// The last glyph does not need its trailing blank column.
			return (static_cast<int>(text.size()) * ADVANCE - 1) * std::max(scale, 1);
		}

		int LineHeight(int scale) {
			return (GLYPH_HEIGHT + 2) * std::max(scale, 1);
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
