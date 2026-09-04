#pragma once

// arch-waiver public-header: forward studio API. Script tooling shares this
// complete code-metrics contract.

// The script panel's monospace arithmetic: bytes to rendered cells and back,
// and the scaling the minimap draws with.
//
// **Free functions over text, for `Complete.hpp`'s reason.** A cell mapping
// that is one column off shows the tooltip for the word *beside* the one under
// the mouse, and a minimap whose scale drifts scrolls somewhere near where you
// clicked - both look like flakiness rather than like arithmetic, and neither
// needs a window to be wrong. The panel supplies pixels and fonts; everything
// that can be checked without either is here.
//
// **A cell is a rendered column, not a byte.** The two differ in exactly one
// place: imgui draws `\t` as a single glyph four spaces wide (`IM_TABSIZE` in
// `imgui_draw.cpp`, fixed rather than aligned to tab stops), so a byte after a
// tab sits four columns along. Everything in this file counts that way, which
// is what keeps the hover's mouse-to-cell mapping and the completion popup's
// cell-to-pixel mapping inverses of each other on indented lines.
//
// @tier client

#include <cstddef>
#include <string_view>
#include <vector>

namespace studio {

	// How many columns one tab occupies. imgui's `IM_TABSIZE`, restated here
	// because the constant lives in `imgui_internal.h` and this header must
	// not; if a vendor bump changes it, columns drift benignly on tabbed
	// lines and this is the one number to follow.
	inline constexpr size_t TAB_COLUMNS = 4;

	// The rendered column a byte is drawn at, within its own line.
	//
	// @param text   The whole buffer.
	// @param offset A byte offset into it. Past the end answers the column
	//               one past the last line's text.
	// @return The 0-based column, tabs counted as `TAB_COLUMNS`.
	// @since v0.17
	size_t ColumnAt(std::string_view text, size_t offset);

	// The byte drawn at a (line, column) cell, or `npos` when nothing is.
	//
	// **`npos` is the answer for empty space, and that is the point.** A line
	// the file does not have, a column past a line's last character, and a
	// newline itself all answer nothing - which is what lets a hover refuse to
	// appear over text that is not there rather than documenting the nearest
	// word to the mouse.
	//
	// A column inside a tab's span answers the tab's byte, so hovering the gap
	// a tab draws resolves to whitespace rather than to the word after it.
	//
	// @param text   The whole buffer.
	// @param line   0-based line index.
	// @param column 0-based rendered column.
	// @return The byte offset, or `std::string_view::npos`.
	// @since v0.17
	size_t OffsetAtCell(std::string_view text, size_t line, size_t column);

	// One horizontal stripe of a minimap line.
	//
	// @since v0.17
	struct MinimapRun {
		// The rendered column the run starts at.
		size_t Column = 0;

		// How many rendered columns it covers.
		size_t Columns = 0;

		// Whether the run is identifier characters, drawn brighter than
		// punctuation. **Two classes and not a highlighter**: the code field
		// tints nothing, so the minimap is a density map by character class
		// rather than a shrunken copy of colours the editor does not have.
		bool Word = false;
	};

	// Splits one line into the stripes the minimap draws.
	//
	// Whitespace separates runs and draws nothing; anything past `maxColumns`
	// is cut, because a minimap column has a width and a line does not.
	//
	// @param line       One line, without its newline.
	// @param maxColumns Where to stop.
	// @param into       Filled with the runs. Cleared first, and passed in so
	//                   a caller drawing hundreds of lines reuses one buffer
	//                   instead of allocating per line per frame.
	// @since v0.17
	void MinimapRunsOf(std::string_view line, size_t maxColumns, std::vector<MinimapRun> &into);

	// The height one file line gets in the minimap.
	//
	// `preferred` until the file no longer fits, then compressed so the whole
	// file always spans the map - which is what makes it a map rather than a
	// second scrollbar.
	//
	// @param lines     How many lines the file has.
	// @param mapHeight The map's height in pixels.
	// @param preferred The height of one line when there is room.
	// @return The row height in pixels. Never zero or negative.
	// @since v0.17
	float MinimapRowHeight(size_t lines, float mapHeight, float preferred);

	// The code field scroll that puts a picked line mid-view.
	//
	// @param pickedLine 0-based line the click or drag landed on.
	// @param lines      How many lines the file has.
	// @param rowHeight  One text row's height in the code field, in pixels.
	// @param viewHeight The code field's visible height, in pixels.
	// @return The scroll, clamped to what the field can actually show.
	// @since v0.17
	float MinimapScrollFor(float pickedLine, size_t lines, float rowHeight, float viewHeight);

}
