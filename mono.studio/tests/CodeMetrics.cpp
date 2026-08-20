// The script panel's cell and minimap arithmetic, which fails silently.
//
// A cell mapping that is off by one shows the hover for the word beside the
// one under the mouse, and a minimap scale that drifts scrolls near where you
// clicked instead of to it - both read as flakiness, neither needs a window to
// happen, and a screenshot of either looks plausible. The panel supplies
// pixels; everything below is checked here without any.
//
// The one outside constraint worth restating: imgui draws `\t` as a single
// glyph four spaces wide (`IM_TABSIZE`), so a rendered column is not a byte
// count on any indented line. These cases are what notice if a vendor bump
// changes that.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <studio/CodeMetrics.hpp>
#include <vector>

TEST_SUITE_ID("studio.codemetrics")

using studio::ColumnAt;
using studio::MinimapRowHeight;
using studio::MinimapRun;
using studio::MinimapRunsOf;
using studio::MinimapScrollFor;
using studio::OffsetAtCell;
using studio::TAB_COLUMNS;

namespace {

	constexpr size_t NONE = std::string_view::npos;

}

TEST_CASE("a byte's rendered column counts tabs as imgui draws them", "[studio][codemetrics]") {
	SECTION("a plain line is byte for byte") {
		const std::string_view text = "local x = 1";
		CHECK(ColumnAt(text, 0) == 0);
		CHECK(ColumnAt(text, 6) == 6);
	}

	SECTION("a tab is four columns") {
		const std::string_view text = "\tlocal x";
		CHECK(ColumnAt(text, 0) == 0);
		CHECK(ColumnAt(text, 1) == TAB_COLUMNS);
		CHECK(ColumnAt(text, 3) == TAB_COLUMNS + 2);
	}

	SECTION("columns restart on every line") {
		const std::string_view text = "abc\ndef";
		CHECK(ColumnAt(text, 4) == 0);
		CHECK(ColumnAt(text, 6) == 2);
	}

	SECTION("past the end is the column after the last character") {
		const std::string_view text = "ab";
		CHECK(ColumnAt(text, 99) == 2);
	}
}

TEST_CASE("a cell maps back to the byte drawn in it, or to nothing", "[studio][codemetrics]") {
	const std::string_view text = "local x\n\tif y then\n\nend";

	SECTION("a plain cell answers its byte") {
		CHECK(OffsetAtCell(text, 0, 0) == 0);
		CHECK(OffsetAtCell(text, 0, 6) == 6);
	}

	SECTION("every column of a tab's span answers the tab") {
		for (size_t column = 0; column < TAB_COLUMNS; column++) {
			CHECK(OffsetAtCell(text, 1, column) == 8);
		}
		CHECK(OffsetAtCell(text, 1, TAB_COLUMNS) == 9);
	}

	SECTION("empty space answers nothing") {
		// Past a line's last character, on an empty line, and past the file.
		CHECK(OffsetAtCell(text, 0, 7) == NONE);
		CHECK(OffsetAtCell(text, 0, 99) == NONE);
		CHECK(OffsetAtCell(text, 2, 0) == NONE);
		CHECK(OffsetAtCell(text, 9, 0) == NONE);
	}

	SECTION("the two directions are inverses over every drawn byte") {
		size_t line = 0;
		for (size_t offset = 0; offset < text.size(); offset++) {
			if (text[offset] == '\n') {
				line++;
				continue;
			}
			CHECK(OffsetAtCell(text, line, ColumnAt(text, offset)) == offset);
		}
	}
}

TEST_CASE("a line becomes the stripes the minimap draws", "[studio][codemetrics]") {
	std::vector<MinimapRun> runs;

	SECTION("words and punctuation split, whitespace draws nothing") {
		MinimapRunsOf("local x = 1", 100, runs);

		REQUIRE(runs.size() == 4);
		CHECK((runs[0].Column == 0 && runs[0].Columns == 5 && runs[0].Word));
		CHECK((runs[1].Column == 6 && runs[1].Columns == 1 && runs[1].Word));
		CHECK((runs[2].Column == 8 && runs[2].Columns == 1 && !runs[2].Word));
		CHECK((runs[3].Column == 10 && runs[3].Columns == 1 && runs[3].Word));
	}

	SECTION("a tab indents the first run by its rendered width") {
		MinimapRunsOf("\tend", 100, runs);

		REQUIRE(runs.size() == 1);
		CHECK(runs[0].Column == TAB_COLUMNS);
		CHECK(runs[0].Columns == 3);
	}

	SECTION("adjacent same-class characters merge into one run") {
		MinimapRunsOf("a==b", 100, runs);

		REQUIRE(runs.size() == 3);
		CHECK((runs[1].Column == 1 && runs[1].Columns == 2 && !runs[1].Word));
	}

	SECTION("the cut falls at the column budget") {
		MinimapRunsOf("abcdefgh", 4, runs);

		REQUIRE(runs.size() == 1);
		CHECK(runs[0].Columns == 4);
	}

	SECTION("the buffer is cleared before reuse") {
		MinimapRunsOf("abc", 100, runs);
		MinimapRunsOf("", 100, runs);
		CHECK(runs.empty());
	}
}

TEST_CASE("the minimap's scale compresses only when the file demands it", "[studio][codemetrics]") {
	SECTION("a short file keeps the preferred row height") {
		CHECK(MinimapRowHeight(100, 600.0f, 2.0f) == 2.0f);
	}

	SECTION("a long file is compressed to always span the map") {
		CHECK(MinimapRowHeight(1200, 600.0f, 2.0f) == 0.5f);
	}

	SECTION("an empty file does not divide by zero") {
		CHECK(MinimapRowHeight(0, 600.0f, 2.0f) == 2.0f);
	}
}

TEST_CASE("a minimap pick centres the line and stays inside the file", "[studio][codemetrics]") {
	// 100 lines of 10px in a 200px view: content is 1000px, scroll tops out
	// at 800.
	SECTION("a middle pick puts the line mid-view") {
		CHECK(MinimapScrollFor(50.0f, 100, 10.0f, 200.0f) == 400.0f);
	}

	SECTION("the top clamps to zero") {
		CHECK(MinimapScrollFor(0.0f, 100, 10.0f, 200.0f) == 0.0f);
	}

	SECTION("the bottom clamps to the last full view") {
		CHECK(MinimapScrollFor(99.0f, 100, 10.0f, 200.0f) == 800.0f);
	}

	SECTION("a file shorter than the view never scrolls") {
		CHECK(MinimapScrollFor(3.0f, 5, 10.0f, 200.0f) == 0.0f);
	}
}
