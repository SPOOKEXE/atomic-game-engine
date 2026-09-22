// Markup in a label, and the two rules that make it safe.
//
// Spans carry style into canonical shaping; bounded malformed markup stays
// literal so an author can see the error.

#include <engine/gui/Components.hpp>
#include <engine/gui/RichText.hpp>
#include <engine/gui/ShapedText.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.gui.richtext")

using Catch::Approx;
using engine::gui::DrawSpan;
using engine::gui::FontFace;
using engine::gui::Label;
using engine::gui::ParseRichText;

namespace {
	// The label a span inherits from, with values nothing here produces by
	// accident - a span that matched the base would not be emitted at all, and a
	// case asserting on one would then be asserting on the absence of a bug it
	// had not introduced.
	Label Base() {
		Label label;
		label.Color = engine::core::Color3{0.25f, 0.5f, 0.75f};
		label.Size = 20;
		label.Font = FontFace::Regular;
		return label;
	}
}

TEST_CASE("plain text parses to itself and no spans", "[gui][richtext]") {
	// The ordinary case, and the one that has to cost a backend nothing: a run
	// with no spans is drawn exactly as it was before markup existed.
	std::string plain;
	std::vector<DrawSpan> spans;

	CHECK(ParseRichText("Score: 1200", Base(), plain, spans));
	CHECK(plain == "Score: 1200");
	CHECK(spans.empty());
}

TEST_CASE("a tag becomes a range over the stripped text", "[gui][richtext]") {
	// **The offsets are into the *stripped* string**, which is the whole
	// contract: the backend receives that string and looks up each byte it draws.
	// Offsets into the authored string would point past the end of it.
	std::string plain;
	std::vector<DrawSpan> spans;

	REQUIRE(ParseRichText("a <b>big</b> word", Base(), plain, spans));
	CHECK(plain == "a big word");
	REQUIRE(spans.size() == 1);
	CHECK(spans[0].Begin == 2);
	CHECK(spans[0].End == 5);
	CHECK(spans[0].Font == FontFace::Bold);
	CHECK(plain.substr(spans[0].Begin, spans[0].End - spans[0].Begin) == "big");
}

TEST_CASE("nested tags inherit and unwind", "[gui][richtext]") {
	// A stack rather than a flag set: closing the inner tag has to put the
	// weight back without touching the colour the outer one set.
	std::string plain;
	std::vector<DrawSpan> spans;

	REQUIRE(ParseRichText(R"(<font color="#FF0000">red <b>bold</b> red</font>)", Base(), plain, spans));
	CHECK(plain == "red bold red");
	REQUIRE(spans.size() == 3);

	CHECK(spans[0].Tint.R == Approx(1.0f));
	CHECK(spans[0].Font == FontFace::Regular);

	CHECK(spans[1].Tint.R == Approx(1.0f));
	CHECK(spans[1].Font == FontFace::Bold);

	CHECK(spans[2].Tint.R == Approx(1.0f));
	CHECK(spans[2].Font == FontFace::Regular);

	// Contiguous and in order, which is what `DrawSpan` promises a backend.
	CHECK(spans[0].End == spans[1].Begin);
	CHECK(spans[1].End == spans[2].Begin);
}

TEST_CASE("font attributes are read and unknown ones are ignored", "[gui][richtext]") {
	std::string plain;
	std::vector<DrawSpan> spans;

	REQUIRE(
		ParseRichText(R"(<font size="32" transparency="0.5" weight="900">x</font>)", Base(), plain, spans)
	);
	CHECK(plain == "x");
	REQUIRE(spans.size() == 1);
	CHECK(spans[0].Size == 32);
	CHECK(spans[0].Transparency == Approx(0.5f));

	// **An unknown *attribute* is ignored and an unknown *tag* is refused**, and
	// the asymmetry is deliberate: an attribute this engine has no answer for
	// still leaves the words readable, where a tag it cannot open would leave
	// the rest of the string styled by something nobody wrote.
	CHECK_FALSE(ParseRichText("<bold>x</bold>", Base(), plain, spans));
	CHECK(plain == "<bold>x</bold>");
	CHECK(spans.empty());
}

TEST_CASE("entities and breaks reach the text", "[gui][richtext]") {
	std::string plain;
	std::vector<DrawSpan> spans;

	REQUIRE(ParseRichText("a &lt; b &amp;&#37; c<br />d", Base(), plain, spans));
	CHECK(plain == "a < b &% c\nd");

	// A bare ampersand is not an entity and is not an error - it is what an
	// author writing "Tom & Jerry" typed.
	REQUIRE(ParseRichText("Tom & Jerry", Base(), plain, spans));
	CHECK(plain == "Tom & Jerry");
	CHECK(ParseRichText("&#55296; & untouched", Base(), plain, spans));
	CHECK(plain == "&#55296; & untouched");
}

TEST_CASE("bare ampersands stay bounded and literal", "[gui][richtext]") {
	std::string plain;
	std::vector<DrawSpan> spans;
	const std::string source(32 * 1024, '&');
	REQUIRE(ParseRichText(source, Base(), plain, spans));
	CHECK(plain == source);
	CHECK(spans.empty());
}

TEST_CASE("malformed markup is shown rather than swallowed", "[gui][richtext]") {
	// **The failure mode that matters.** Half a parse is worse than none: a
	// reader shown a partly-stripped string cannot tell whether the markup ran
	// out or the text did, and an author cannot see where the mistake is.
	std::string plain;
	std::vector<DrawSpan> spans;

	for (const std::string_view broken : {
			 std::string_view("<b>never closed"),
			 std::string_view("closed too often</b>"),
			 std::string_view("<b>unterminated tag"),
			 std::string_view("a < b"),
			 std::string_view(R"(<font color="nonsense">x</font>)"),
		 }) {
		INFO("source: " << broken);
		CHECK_FALSE(ParseRichText(broken, Base(), plain, spans));
		CHECK(plain == broken);
		CHECK(spans.empty());
	}
}

TEST_CASE("rich text refuses oversized input before copying it", "[gui][richtext]") {
	std::string plain = "previous";
	std::vector<DrawSpan> spans(1);
	const std::string source(engine::gui::MAXIMUM_SHAPED_TEXT_BYTES + 1, 'x');
	CHECK_FALSE(ParseRichText(source, Base(), plain, spans));
	CHECK(plain.empty());
	CHECK(spans.empty());
}

TEST_CASE("rich text bounds nesting and styled runs", "[gui][richtext]") {
	std::string plain;
	std::vector<DrawSpan> spans;
	std::string nested;
	for (int index = 0; index < 64; ++index)
		nested += "<b>";
	nested += 'x';
	for (int index = 0; index < 64; ++index)
		nested += "</b>";
	CHECK_FALSE(ParseRichText(nested, Base(), plain, spans));
	CHECK(plain == nested);
	CHECK(spans.empty());

	std::string manySpans;
	for (int index = 0; index < 65; ++index)
		manySpans += "<b>x</b>";
	CHECK_FALSE(ParseRichText(manySpans, Base(), plain, spans));
	CHECK(plain == manySpans);
	CHECK(spans.empty());
}

TEST_CASE("the visible limit counts characters and never splits one", "[gui][richtext]") {
	// `Label::MaxVisible` is a typewriter's counter, and counting bytes is what
	// makes one reveal half of an accented letter.
	using engine::gui::FirstCharacters;

	CHECK(FirstCharacters("abcdef", -1) == "abcdef");
	CHECK(FirstCharacters("abcdef", 0).empty());
	CHECK(FirstCharacters("abcdef", 3) == "abc");
	CHECK(FirstCharacters("abcdef", 99) == "abcdef");

	// "héllo" - the accented letter is two bytes, so a byte count of two would
	// cut it in half and draw a replacement glyph.
	constexpr std::string_view accented = "h\xC3\xA9llo";
	CHECK(FirstCharacters(accented, 2) == "h\xC3\xA9");
	CHECK(FirstCharacters(accented, 1) == "h");
}
