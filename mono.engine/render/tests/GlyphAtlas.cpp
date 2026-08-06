// The four faces, rasterised once, in one place.
//
// **This is the roadmap's open decision made concrete**: "the atlas is the
// decision inside it, unchanged: whichever module ends up owning the four
// faces, only one does". `ui` owned them through imgui, which was right while
// the editor was the only thing drawing text and stops being right the moment a
// shipped client has to draw a `ScreenGui` — `mono.client` does not link
// `Engine::ui` and must not.
//
// These cases run headless. Rasterising needs no device, which is what makes
// the atlas testable at all and is a reason to have built it before the
// pipeline that uploads it: a wrong glyph here would otherwise be found by
// looking at a screen.

#include <engine/core/Paths.hpp>
#include <engine/render/GlyphAtlas.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

TEST_SUITE_ID("engine.render.glyphatlas")

using engine::render::Glyph;
using engine::render::GlyphAtlas;
using engine::render::Typeface;

namespace {
	// The staged assets, which is where the fonts are.
	// The fonts are staged beside the test binary, the same way a program's are
	// staged beside it — `mono_add_tests` copies them for a test that links
	// something which reads them.
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base());
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	bool FontsPresent() {
		return std::filesystem::exists(engine::core::Paths::Assets() / "fonts" / "Inter.ttf");
	}
}

TEST_CASE("the atlas bakes every vendored face", "[render][glyphatlas]") {
	const StagedAssets assets;
	if (!FontsPresent()) {
		SUCCEED("no staged fonts; the atlas reports that rather than failing");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));
	REQUIRE(atlas.Ready());

	// A power of two, because that is what a sampler wants and what a driver
	// will not quietly re-lay-out.
	CHECK(atlas.Width() == atlas.Height());
	CHECK((atlas.Width() & (atlas.Width() - 1)) == 0);
	CHECK(atlas.Coverage().size() == static_cast<size_t>(atlas.Width()) * atlas.Height());

	// Every face has an 'A' with pixels in it. A face that failed to load
	// silently would leave its whole range empty, which is the failure a
	// per-face check catches and a whole-atlas one does not.
	for (int face = 0; face < static_cast<int>(Typeface::Count); face++) {
		INFO("face " << face);
		const Glyph *letter = atlas.Find(static_cast<Typeface>(face), U'A');
		REQUIRE(letter != nullptr);
		CHECK(letter->Width > 0);
		CHECK(letter->Height > 0);
		CHECK(letter->Advance > 0.0f);
	}

	// **At least the em size, and these faces report exactly it.**
	//
	// `LineHeight` is the font's own metric — ascent less descent plus the line
	// gap — and all four vendored faces declare a zero gap, so it comes out at
	// precisely the size asked for. That reads as "consecutive lines touch",
	// and it is the honest number rather than a wrong one: the *spacing* a
	// layout leaves between lines is `gui::LINE_SPACING`, which is 1.2 and is
	// the engine's choice rather than the typeface's.
	//
	// Reporting 1.2x here instead would put that decision in two places, and
	// the two would disagree the first time one was tuned.
	CHECK(atlas.LineHeight() >= 16.0f);
}

TEST_CASE("a space has an advance and no pixels", "[render][glyphatlas]") {
	// **Metrics without an outline is not a failure**, and an atlas that
	// treated it as one would drop the one character every run of text
	// contains. It keeps its advance and takes no room in the sheet.
	const StagedAssets assets;
	if (!FontsPresent()) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	const Glyph *space = atlas.Find(Typeface::Interface, U' ');
	REQUIRE(space != nullptr);
	CHECK(space->Advance > 0.0f);
	CHECK(space->Width == 0);
}

TEST_CASE("a codepoint outside the baked range resolves to nothing", "[render][glyphatlas]") {
	// **Null rather than a zero-size box**, so a caller draws its missing-glyph
	// marker — visible on purpose, for `ui::ImageSource`'s reason: text that
	// silently vanished would look like the label was broken rather than like
	// the character was unavailable.
	const StagedAssets assets;
	if (!FontsPresent()) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	CHECK(atlas.Find(Typeface::Interface, U'中') == nullptr);
	CHECK(atlas.Find(Typeface::Interface, 0) == nullptr);
	CHECK(atlas.Find(Typeface::Count, U'A') == nullptr);
}

TEST_CASE("glyphs are padded so a sampler cannot read a neighbour", "[render][glyphatlas]") {
	// **The classic atlas bleed, and it only shows at non-integer scales** —
	// which is to say on somebody else's machine. A sampler filtering at the
	// edge of a packed glyph reads the neighbour's coverage and draws a faint
	// smear of an unrelated letter down one side.
	//
	// Checked by looking at the sheet rather than by trusting the constant: the
	// column immediately left of every glyph must be empty.
	const StagedAssets assets;
	if (!FontsPresent()) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	const std::vector<uint8_t> &pixels = atlas.Coverage();
	const uint32_t width = atlas.Width();

	size_t checked = 0;
	for (char32_t codepoint = U'A'; codepoint <= U'Z'; codepoint++) {
		const Glyph *glyph = atlas.Find(Typeface::Interface, codepoint);
		if (glyph == nullptr || glyph->X == 0 || glyph->Width == 0) {
			continue;
		}

		for (uint16_t row = 0; row < glyph->Height; row++) {
			const size_t left =
				static_cast<size_t>(glyph->Y + row) * width + static_cast<size_t>(glyph->X) - 1;
			REQUIRE(left < pixels.size());
			CHECK(pixels[left] == 0);
		}
		checked++;
	}

	// Asserted so that a change making every glyph zero-width could not pass
	// this by checking nothing.
	CHECK(checked > 20);
}

TEST_CASE("measuring text is the sum of its advances", "[render][glyphatlas]") {
	// **The real number, against `gui::AVERAGE_ADVANCE`'s estimate.** That
	// constant exists because `gui` is `shared` and cannot rasterise anything;
	// this is what a backend that *has* an atlas should use, and the estimate is
	// there so the two do not disagree about where an element is — not so the
	// text stays wrong.
	const StagedAssets assets;
	if (!FontsPresent()) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	CHECK(atlas.Measure(Typeface::Interface, "") == 0.0f);

	const float one = atlas.Measure(Typeface::Interface, "M");
	const float two = atlas.Measure(Typeface::Interface, "MM");
	CHECK(one > 0.0f);
	CHECK(two > one);

	// **Monospace is the face where a wrong sum is provable rather than
	// plausible.** Every advance is identical, so ten characters must be
	// exactly ten times one — a measurement that dropped or double-counted a
	// glyph fails here and would be invisible in a proportional face.
	const float single = atlas.Measure(Typeface::Monospace, "x");
	const float ten = atlas.Measure(Typeface::Monospace, "xxxxxxxxxx");
	CHECK(std::abs(ten - single * 10.0f) < 0.01f);
}

TEST_CASE("a bad size is refused rather than baked", "[render][glyphatlas]") {
	GlyphAtlas atlas;
	CHECK_FALSE(atlas.Build(0.0f));
	CHECK_FALSE(atlas.Ready());
	CHECK_FALSE(atlas.Build(-8.0f));
}

TEST_CASE("the white texel is packed rather than assumed", "[render][glyphatlas]") {
	// **A filled rectangle and a glyph go through one pipeline**, which needs
	// one solid texel to sample. Two pipelines — one textured, one not — is two
	// places for the blend state to be set differently, and that shows as
	// interface panels being subtly the wrong opacity and nowhere else.
	//
	// **The texel takes a rect from the packer rather than a corner somebody
	// picked.** `stbrp` fills the whole sheet, so a hand-picked texel is one a
	// large glyph can land on — putting a letter's coverage under every filled
	// rectangle in the interface. That bug appears only once the atlas is full
	// enough for that glyph to go there, which is to say on the machine with the
	// wider font, which is to say not this one.
	//
	// So this checks the texel is actually solid *and* that its neighbours are
	// empty, which is what a packed-and-padded rect guarantees and a corner does
	// not.
	const StagedAssets assets;
	if (!FontsPresent()) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	const engine::core::Vector2 uv = atlas.WhiteTexel();
	CHECK(uv.X > 0.0f);
	CHECK(uv.Y > 0.0f);
	CHECK(uv.X < 1.0f);
	CHECK(uv.Y < 1.0f);

	// Back to texels. The UV names the centre, so truncating lands on it.
	const auto x = static_cast<uint32_t>(uv.X * static_cast<float>(atlas.Width()));
	const auto y = static_cast<uint32_t>(uv.Y * static_cast<float>(atlas.Height()));

	const std::vector<uint8_t> &pixels = atlas.Coverage();
	CHECK(pixels[static_cast<size_t>(y) * atlas.Width() + x] == 255);

	// **Padded like any glyph**, so a sampler filtering at its edge cannot read
	// a neighbour and draw a solid quad at partial alpha.
	CHECK(pixels[static_cast<size_t>(y) * atlas.Width() + x - 1] == 0);
	CHECK(pixels[static_cast<size_t>(y) * atlas.Width() + x + 1] == 0);
	CHECK(pixels[static_cast<size_t>(y - 1) * atlas.Width() + x] == 0);
	CHECK(pixels[static_cast<size_t>(y + 1) * atlas.Width() + x] == 0);

	// And it is not mistaken for a glyph, which the slot offset is what
	// prevents — a reader that forgot it would return the white texel for the
	// first baked codepoint.
	const Glyph *space = atlas.Find(Typeface::Interface, GlyphAtlas::FIRST_CODEPOINT);
	REQUIRE(space != nullptr);
	CHECK(space->Width == 0);
}
