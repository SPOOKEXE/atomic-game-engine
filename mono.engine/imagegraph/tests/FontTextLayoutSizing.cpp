#include "FontTextLayout.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.font_text_layout_sizing")
using namespace engine::imagegraph;

TEST_CASE("source full-text monospace width overrides proportional observations only without wrap") {
	FontData font;
	font.GlyphMapComplete = true;
	font.Characters = FontCharacterProfile::UnicodeScalar;
	font.LineHeight = 10;
	font.Glyphs = {{65, true, {}, 3, 3, 10, {}, {}, 0}, {87, true, {}, 8, 8, 10, {}, {}, 0}};
	font.Measurements = {{"A\nA", 0, -1, 3, 20}, {"A\nA", 16, -1, 3, 20}};
	detail::FontTextLayoutOptions options;
	options.FullTextSize = true;
	options.Monospaced = true;
	options.Trim = true;
	options.Range = {0, 1.0 / 3};
	detail::FontTextLayout result;
	std::string failure;
	REQUIRE(
		detail::BuildFontTextLayout(font, "A\nA", options, Limits::MaximumEvaluationBytes, result, failure) ==
		Status::Ok
	);
	CHECK(result.Text == "A");
	CHECK(result.Width == 24);
	CHECK(result.Height == 20);

	options.MaximumLineWidth = 16;
	REQUIRE(
		detail::BuildFontTextLayout(font, "A\nA", options, Limits::MaximumEvaluationBytes, result, failure) ==
		Status::Ok
	);
	CHECK(result.Width == 3);
	CHECK(result.Height == 20);

	options.MaximumLineWidth = 0;
	options.Monospaced = false;
	REQUIRE(
		detail::BuildFontTextLayout(font, "A\nA", options, Limits::MaximumEvaluationBytes, result, failure) ==
		Status::Ok
	);
	CHECK(result.Width == 3);
}

TEST_CASE("source full-text monospace count follows expanded casing before trimming") {
	FontData font;
	font.GlyphMapComplete = true;
	font.LineHeight = 10;
	font.Glyphs = {{87, true, {}, 8, 8, 10, {}, {}, 0}};
	font.Measurements = {{"SS", 0, -1, 11, 10}};
	detail::FontTextLayoutOptions options;
	options.ChangeCase = 2;
	options.ObservedCasedText = "SS";
	options.FullTextSize = true;
	options.Monospaced = true;
	options.Trim = true;
	options.Range = {0, 0.5};
	detail::FontTextLayout result;
	std::string failure;
	REQUIRE(
		detail::BuildFontTextLayout(font, "ß", options, Limits::MaximumEvaluationBytes, result, failure) ==
		Status::Ok
	);
	CHECK(result.RawText == "SS");
	CHECK(result.Text == "S");
	CHECK(result.Width == 16);
	CHECK(result.Height == 10);
}
