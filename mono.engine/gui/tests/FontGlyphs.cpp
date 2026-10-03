#include "FontGlyphBitmap.hpp"

#include <engine/core/Paths.hpp>
#include <engine/gui/FontGlyphs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>

TEST_SUITE_ID("engine.gui.fontglyphs")

using namespace engine::gui;

namespace {
	std::span<const std::byte> FontBytes(std::string_view font) {
		return {reinterpret_cast<const std::byte *>(font.data()), font.size()};
	}
	constexpr std::string_view BITMAP_FONT = R"(STARTFONT 2.1
FONT -engine-test-medium-r-normal--10-100-72-72-c-80-iso10646-1
SIZE 10 72 72
FONTBOUNDINGBOX 5 7 -1 -2
STARTPROPERTIES 4
FONT_ASCENT 8
FONT_DESCENT 2
CHARSET_REGISTRY "ISO10646"
CHARSET_ENCODING "1"
ENDPROPERTIES
CHARS 2
STARTCHAR A
ENCODING 65
SWIDTH 800 0
DWIDTH 8 0
BBX 5 7 -1 -2
BITMAP
70
88
88
F8
88
88
88
ENDCHAR
STARTCHAR space
ENCODING 32
SWIDTH 400 0
DWIDTH 4 0
BBX 0 0 0 0
BITMAP
ENDCHAR
ENDFONT
)";
	FontGlyphBatch Sentinel() {
		FontGlyphBatch output;
		FontGlyphCoverage glyph;
		glyph.Character = 123;
		glyph.Coverage = {19, 23};
		output.Glyphs.push_back(std::move(glyph));
		output.LineHeightPixels = 9;
		output.RetainedBytes = 27;
		output.PeakOperationBytes = 28;
		return output;
	}
	void CheckSentinel(const FontGlyphBatch &output) {
		REQUIRE(output.Glyphs.size() == 1);
		CHECK(output.Glyphs[0].Character == 123);
		CHECK(output.Glyphs[0].Coverage == std::vector<uint8_t>{19, 23});
		CHECK(output.LineHeightPixels == 9);
		CHECK(output.RetainedBytes == 27);
		CHECK(output.PeakOperationBytes == 28);
	}
	std::vector<std::byte> DeliveredFont() {
		const auto path = engine::core::Paths::Base() / "fonts" / "Inter.ttf";
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		REQUIRE(file.good());
		const auto bytes = file.tellg();
		REQUIRE(bytes > 0);
		REQUIRE(bytes <= std::streamoff(MAXIMUM_FONT_GLYPH_FILE_BYTES));
		std::vector<std::byte> font(static_cast<size_t>(bytes));
		file.seekg(0);
		REQUIRE(file.read(reinterpret_cast<char *>(font.data()), bytes).good());
		return font;
	}
}

TEST_CASE(
	"font glyph decoding returns exact bitmap bearings, advances and real line metrics", "[gui][font_glyphs]"
) {
	const std::array<uint32_t, 3> characters{65, 32, 0x10ffff};
	FontGlyphBatch output;
	REQUIRE(DecodeFontGlyphs({FontBytes(BITMAP_FONT), characters, 10, false}, output) == FontGlyphStatus::Ok);
	REQUIRE(output.Glyphs.size() == 3);
	CHECK(output.AscentPixels == 8);
	CHECK(output.DescentPixels == 2);
	CHECK(output.LineHeightPixels == 10);
	const auto &glyph = output.Glyphs[0];
	REQUIRE(glyph.Present);
	CHECK(glyph.Character == 65);
	CHECK(glyph.GlyphIndex != 0);
	CHECK(glyph.AdvanceXPixels == 8);
	CHECK(glyph.AdvanceYPixels == 0);
	CHECK(glyph.OffsetXPixels == -1);
	CHECK(glyph.OffsetYPixels == -5);
	CHECK(glyph.Width == 5);
	CHECK(glyph.Height == 7);
	CHECK(glyph.Coverage == std::vector<uint8_t>{0,	  255, 255, 255, 0,	  255, 0,	0,	 0,	  255, 255, 0,
												 0,	  0,   255, 255, 255, 255, 255, 255, 255, 0,   0,	0,
												 255, 255, 0,	0,	 0,	  255, 255, 0,	 0,	  0,   255});
	const auto &space = output.Glyphs[1];
	CHECK(space.Present);
	CHECK(space.AdvanceXPixels == 4);
	CHECK(space.Coverage.empty());
	const auto &missing = output.Glyphs[2];
	CHECK_FALSE(missing.Present);
	CHECK(missing.GlyphIndex == 0);
	CHECK(missing.AdvanceXPixels == 0);
	CHECK(missing.Coverage.empty());
	CHECK(
		output.RetainedBytes ==
		output.Glyphs.capacity() * sizeof(FontGlyphCoverage) + glyph.Coverage.capacity()
	);
	CHECK(output.PeakOperationBytes >= output.RetainedBytes + BITMAP_FONT.size());
	CHECK(output.PeakOperationBytes <= MAXIMUM_FONT_GLYPH_OPERATION_BYTES);
}

TEST_CASE(
	"outline fonts preserve requested order and real mono or antialiased coverage", "[gui][font_glyphs]"
) {
	const auto bytes = DeliveredFont();
	const std::array<uint32_t, 4> characters{'W', ' ', 'i', 0x10ffff};
	for (bool antialias : {false, true}) {
		FontGlyphBatch output;
		REQUIRE(DecodeFontGlyphs({bytes, characters, 24, antialias}, output) == FontGlyphStatus::Ok);
		REQUIRE(output.Glyphs.size() == characters.size());
		CHECK(output.LineHeightPixels > 0);
		CHECK(output.AscentPixels > 0);
		CHECK(output.DescentPixels >= 0);
		for (size_t i = 0; i < characters.size(); ++i)
			CHECK(output.Glyphs[i].Character == characters[i]);
		CHECK(output.Glyphs[0].AdvanceXPixels > output.Glyphs[2].AdvanceXPixels);
		CHECK(output.Glyphs[1].AdvanceXPixels > 0);
		// FreeType expands a collapsed monochrome outline box to one transparent pixel.
		const auto &space = output.Glyphs[1];
		REQUIRE(space.Present);
		CHECK(space.OffsetXPixels == 0);
		if (antialias) {
			CHECK(space.Width == 0);
			CHECK(space.Height == 0);
			CHECK(space.OffsetYPixels == 0);
			CHECK(space.Coverage.empty());
		} else {
			CHECK(space.Width == 1);
			CHECK(space.Height == 1);
			CHECK(space.OffsetYPixels == -1);
			CHECK(space.Coverage == std::vector<uint8_t>{0});
		}
		CHECK_FALSE(output.Glyphs.back().Present);
		const auto &coverage = output.Glyphs[0].Coverage;
		REQUIRE_FALSE(coverage.empty());
		CHECK(std::any_of(coverage.begin(), coverage.end(), [](auto alpha) { return alpha == 255; }));
		const bool fractional = std::any_of(coverage.begin(), coverage.end(), [](auto alpha) {
			return alpha > 0 && alpha < 255;
		});
		CHECK(fractional == antialias);
	}
}

TEST_CASE(
	"glyph coverage handles signed pitch, row padding and gray normalization before publication",
	"[gui][font_glyphs]"
) {
	const std::array<uint8_t, 6> storage{0, 3, 91, 1, 2, 92};
	std::array<uint8_t, 4> output{};
	REQUIRE(detail::CopyFontGlyphBitmap({storage, 2, 2, -3, 4, false}, output));
	CHECK(output == std::array<uint8_t, 4>{85, 170, 0, 255});
	const std::array<uint8_t, 4> mono{0x80, 73, 0x40, 74};
	REQUIRE(detail::CopyFontGlyphBitmap({mono, 2, 2, 2, 0, true}, output));
	CHECK(output == std::array<uint8_t, 4>{255, 0, 0, 255});
	output = {17, 18, 19, 20};
	CHECK_FALSE(detail::CopyFontGlyphBitmap({storage, 3, 2, 2, 4, false}, output));
	CHECK(output == std::array<uint8_t, 4>{17, 18, 19, 20});
	const std::array<uint8_t, 4> invalidGray{0, 1, 2, 4};
	CHECK_FALSE(detail::CopyFontGlyphBitmap({invalidGray, 2, 2, 2, 4, false}, output));
	CHECK(output == std::array<uint8_t, 4>{17, 18, 19, 20});
}

TEST_CASE(
	"font requests reject invalid Unicode and duplicates without replacing existing glyphs",
	"[gui][font_glyphs]"
) {
	for (const auto characters : {std::array<uint32_t, 2>{65, 65}, {65, 0xd800}, {65, 0x110000}}) {
		auto output = Sentinel();
		CHECK(
			DecodeFontGlyphs({FontBytes(BITMAP_FONT), characters, 10}, output) ==
			FontGlyphStatus::InvalidRequest
		);
		CheckSentinel(output);
	}
	const std::array<uint32_t, 1> character{65};
	for (uint16_t size : {uint16_t{0}, uint16_t{513}}) {
		auto output = Sentinel();
		CHECK(
			DecodeFontGlyphs({FontBytes(BITMAP_FONT), character, size}, output) ==
			FontGlyphStatus::InvalidRequest
		);
		CheckSentinel(output);
	}
	const std::array<uint32_t, MAXIMUM_FONT_GLYPH_CHARACTERS + 1> tooMany{};
	auto output = Sentinel();
	CHECK(DecodeFontGlyphs({FontBytes(BITMAP_FONT), tooMany, 10}, output) == FontGlyphStatus::LimitExceeded);
	CheckSentinel(output);
}

TEST_CASE(
	"invalid fonts and unbounded external WOFF2 codecs preserve the previous batch", "[gui][font_glyphs]"
) {
	const std::array<uint32_t, 1> character{65};
	for (auto font : {std::string_view{"invalid"}, BITMAP_FONT.substr(0, 17)}) {
		auto output = Sentinel();
		CHECK(DecodeFontGlyphs({FontBytes(font), character, 10}, output) == FontGlyphStatus::InvalidFont);
		CheckSentinel(output);
	}
	auto output = Sentinel();
	CHECK(DecodeFontGlyphs({FontBytes("wOF2"), character, 10}, output) == FontGlyphStatus::UnsupportedFont);
	CheckSentinel(output);
	CHECK(DecodeFontGlyphs({{}, character, 10}, output) == FontGlyphStatus::InvalidRequest);
	CheckSentinel(output);
}

TEST_CASE("vendor workspace and copied coverage share one bounded atomic operation", "[gui][font_glyphs]") {
	const std::array<uint32_t, 1> character{65};
	FontGlyphBatch successful;
	REQUIRE(DecodeFontGlyphs({FontBytes(BITMAP_FONT), character, 10}, successful) == FontGlyphStatus::Ok);
	auto output = Sentinel();
	const size_t inputOnly = BITMAP_FONT.size() + character.size() * sizeof(uint32_t) +
							 MAXIMUM_FONT_GLYPH_CHARACTERS * sizeof(uint32_t);
	CHECK(
		DecodeFontGlyphs({FontBytes(BITMAP_FONT), character, 10, true, inputOnly}, output) ==
		FontGlyphStatus::LimitExceeded
	);
	CheckSentinel(output);
	CHECK(
		DecodeFontGlyphs({FontBytes(BITMAP_FONT), character, 10, true, successful.RetainedBytes}, output) ==
		FontGlyphStatus::LimitExceeded
	);
	CheckSentinel(output);
	const auto bytes = DeliveredFont();
	std::array<uint32_t, MAXIMUM_FONT_GLYPH_CHARACTERS> characters{};
	for (size_t i = 0; i < characters.size(); ++i)
		characters[i] = static_cast<uint32_t>(i);
	CHECK(DecodeFontGlyphs({bytes, characters, 512}, output) == FontGlyphStatus::LimitExceeded);
	CheckSentinel(output);
}

TEST_CASE(
	"empty glyph requests retain real line metrics and fixed strikes refuse unavailable sizes",
	"[gui][font_glyphs]"
) {
	FontGlyphBatch output;
	REQUIRE(DecodeFontGlyphs({FontBytes(BITMAP_FONT), {}, 10}, output) == FontGlyphStatus::Ok);
	CHECK(output.Glyphs.empty());
	CHECK(output.LineHeightPixels == 10);
	CHECK(output.RetainedBytes == 0);
	output = Sentinel();
	CHECK(DecodeFontGlyphs({FontBytes(BITMAP_FONT), {}, 11}, output) == FontGlyphStatus::DecodeFailed);
	CheckSentinel(output);
}

TEST_CASE("font input can borrow the prior output until atomic replacement", "[gui][font_glyphs]") {
	FontGlyphBatch output;
	FontGlyphCoverage previous;
	previous.Character = 65;
	previous.Coverage.assign(BITMAP_FONT.begin(), BITMAP_FONT.end());
	output.Glyphs.push_back(std::move(previous));
	const auto &stored = output.Glyphs.front();
	const std::span<const std::byte> bytes{
		reinterpret_cast<const std::byte *>(stored.Coverage.data()), stored.Coverage.size()
	};
	const std::span<const uint32_t> characters{&stored.Character, 1};
	REQUIRE(DecodeFontGlyphs({bytes, characters, 10, false}, output) == FontGlyphStatus::Ok);
	REQUIRE(output.Glyphs.size() == 1);
	CHECK(output.Glyphs[0].Character == 65);
	CHECK(output.Glyphs[0].AdvanceXPixels == 8);
	CHECK(output.Glyphs[0].Coverage.size() == 35);
}
