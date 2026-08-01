#include <engine/render/Overlay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.overlay")

namespace DebugText = engine::render::DebugText;
using engine::render::OverlayImage;

namespace {
	// The image is RGBA8; this reads one channel back out.
	uint8_t At(const OverlayImage &image, int x, int y, int channel) {
		const size_t index =
			(static_cast<size_t>(y) * static_cast<size_t>(image.GetWidth()) + static_cast<size_t>(x)) *
				OverlayImage::BYTES_PER_PIXEL +
			static_cast<size_t>(channel);
		return image.GetPixels()[index];
	}

	bool AnyPixelSet(const OverlayImage &image) {
		for (size_t index = 0; index < image.GetByteCount(); index++) {
			if (image.GetPixels()[index] != 0) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("a fresh image is empty and clean", "[overlay]") {
	OverlayImage image;
	REQUIRE(image.IsEmpty());
	REQUIRE_FALSE(image.IsDirty());

	image.Resize(64, 32);
	REQUIRE_FALSE(image.IsEmpty());
	REQUIRE(image.GetByteCount() == 64u * 32u * 4u);
	REQUIRE_FALSE(image.IsDirty());
}

TEST_CASE("an opaque blend writes the colour exactly", "[overlay]") {
	OverlayImage image;
	image.Resize(16, 16);

	image.Blend(2, 3, 4, 5, 10, 20, 30, 255);

	REQUIRE(At(image, 2, 3, 0) == 10);
	REQUIRE(At(image, 2, 3, 1) == 20);
	REQUIRE(At(image, 2, 3, 2) == 30);
	REQUIRE(At(image, 2, 3, 3) == 255);

	// Just inside the far corner, and just outside it.
	REQUIRE(At(image, 5, 7, 3) == 255);
	REQUIRE(At(image, 6, 7, 3) == 0);
	REQUIRE(At(image, 5, 8, 3) == 0);
}

TEST_CASE("Fill matches Blend over a transparent destination", "[overlay]") {
	// The entire justification for Fill existing. If these two ever disagree,
	// every debug panel changes colour and the fast path is a bug rather than
	// an optimisation — so it is checked across the alpha range rather than at
	// the one value the panels happen to use.
	for (int alpha = 1; alpha <= 255; alpha++) {
		OverlayImage blended;
		OverlayImage filled;
		blended.Resize(4, 4);
		filled.Resize(4, 4);

		const auto value = static_cast<uint8_t>(alpha);
		blended.Blend(0, 0, 4, 4, 8, 10, 16, value);
		filled.Fill(0, 0, 4, 4, 8, 10, 16, value);

		for (int channel = 0; channel < 4; channel++) {
			REQUIRE(At(filled, 2, 2, channel) == At(blended, 2, 2, channel));
		}
	}
}

TEST_CASE("Fill clips and marks dirty exactly as Blend does", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);

	REQUIRE_FALSE(image.IsDirty());
	image.Fill(-4, -4, 6, 6, 200, 100, 50, 208);
	REQUIRE(image.IsDirty());

	// Off every edge, and past them entirely.
	image.Fill(6, 6, 10, 10, 200, 100, 50, 208);
	image.Fill(100, 100, 4, 4, 200, 100, 50, 208);
	image.Fill(0, 0, 0, 4, 200, 100, 50, 208);

	REQUIRE(At(image, 0, 0, 3) == 208);
	REQUIRE(At(image, 1, 1, 3) == 208);
	REQUIRE(At(image, 3, 3, 3) == 0);
	REQUIRE(At(image, 7, 7, 3) == 208);
}

TEST_CASE("Fill covers every row of the rectangle", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);

	// The first row is written pixel by pixel and the rest are copies of it.
	// A row count off by one leaves the last row of every debug panel
	// transparent, which reads as the panel being one pixel short rather than
	// as a bug in a memcpy.
	image.Fill(1, 1, 5, 5, 200, 100, 50, 255);

	for (int y = 1; y < 6; y++) {
		for (int x = 1; x < 6; x++) {
			REQUIRE(At(image, x, y, 0) == 200);
			REQUIRE(At(image, x, y, 3) == 255);
		}
	}

	// And nothing outside it.
	REQUIRE(At(image, 0, 1, 3) == 0);
	REQUIRE(At(image, 6, 1, 3) == 0);
	REQUIRE(At(image, 1, 0, 3) == 0);
	REQUIRE(At(image, 1, 6, 3) == 0);
}

TEST_CASE("a zero-alpha fill changes nothing", "[overlay]") {
	OverlayImage image;
	image.Resize(4, 4);

	image.Fill(0, 0, 4, 4, 255, 255, 255, 0);

	REQUIRE_FALSE(image.IsDirty());
	REQUIRE(At(image, 1, 1, 3) == 0);
}

TEST_CASE("an opaque blend replaces whatever was under it", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);

	image.Blend(0, 0, 8, 8, 200, 100, 50, 255);
	image.Blend(1, 1, 2, 2, 10, 20, 30, 255);

	// Opaque takes a short path that stores the source instead of computing
	// `(c * 255 + under * 0 + 127) / 255`. Those are the same byte for every c,
	// but only over a *non-empty* destination does the test say so — over a
	// fresh image every wrong answer that ignores the destination still passes.
	REQUIRE(At(image, 1, 1, 0) == 10);
	REQUIRE(At(image, 1, 1, 1) == 20);
	REQUIRE(At(image, 1, 1, 2) == 30);
	REQUIRE(At(image, 1, 1, 3) == 255);

	// And the pixel beside it is untouched.
	REQUIRE(At(image, 3, 1, 0) == 200);
}

TEST_CASE("every lit pixel of a glyph row is drawn", "[overlay]") {
	OverlayImage image;
	image.Resize(16, 16);

	// '0' is 111 / 101 / 101 / 101 / 111, so its top row is three lit pixels
	// side by side. They are emitted as one run rather than three calls, and a
	// run built with the wrong end index loses the last pixel of it — which at
	// scale 1 is one dot nobody would notice until every glyph looked thin.
	DebugText::Draw(image, 0, 0, "0", 255, 255, 255, 1);

	REQUIRE(At(image, 0, 0, 3) == 255);
	REQUIRE(At(image, 1, 0, 3) == 255);
	REQUIRE(At(image, 2, 0, 3) == 255);

	// The middle rows are 101: lit, gap, lit. A run that swallowed the gap
	// would fill the middle of the zero in.
	REQUIRE(At(image, 0, 1, 3) == 255);
	REQUIRE(At(image, 1, 1, 3) == 0);
	REQUIRE(At(image, 2, 1, 3) == 255);
}

TEST_CASE("drawing off the edge is clipped rather than corrupting memory", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);

	// Every caller is allowed to draw off the edge; clipping happens once, in
	// Blend, so that no caller has to check.
	image.Blend(-4, -4, 6, 6, 255, 255, 255, 255);
	image.Blend(6, 6, 10, 10, 255, 255, 255, 255);
	image.Blend(100, 100, 4, 4, 255, 255, 255, 255);
	image.Blend(-50, 0, 4, 4, 255, 255, 255, 255);

	REQUIRE(At(image, 0, 0, 3) == 255);
	REQUIRE(At(image, 1, 1, 3) == 255);
	REQUIRE(At(image, 2, 2, 3) == 0);
	REQUIRE(At(image, 7, 7, 3) == 255);
}

TEST_CASE("a zero-alpha blend changes nothing", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);

	image.Blend(0, 0, 8, 8, 255, 255, 255, 0);

	REQUIRE_FALSE(image.IsDirty());
	REQUIRE_FALSE(AnyPixelSet(image));
}

TEST_CASE("blending white over black at half alpha lands mid-grey", "[overlay]") {
	OverlayImage image;
	image.Resize(4, 4);

	image.Blend(0, 0, 4, 4, 255, 255, 255, 128);

	// 255 * 128 / 255 = 128, and the rounding term must not push it past that.
	REQUIRE(At(image, 0, 0, 0) == 128);
}

TEST_CASE("layered blends accumulate towards the source colour", "[overlay]") {
	OverlayImage image;
	image.Resize(4, 4);

	for (int pass = 0; pass < 8; pass++) {
		image.Blend(0, 0, 4, 4, 255, 255, 255, 128);
	}

	// Without the rounding term in Blend, repeated layering drifts downwards
	// and never reaches white. Eight halvings should be within a step of it.
	REQUIRE(At(image, 0, 0, 0) >= 254);
}

TEST_CASE("dirty tracks whether anything was drawn", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);
	REQUIRE_FALSE(image.IsDirty());

	image.Blend(0, 0, 2, 2, 1, 2, 3, 255);
	REQUIRE(image.IsDirty());

	// The renderer skips the upload and the overlay pass when this is false, so
	// Clear has to reset it.
	image.Clear();
	REQUIRE_FALSE(image.IsDirty());
	REQUIRE_FALSE(AnyPixelSet(image));
}

TEST_CASE("resizing to the same size keeps the contents", "[overlay]") {
	OverlayImage image;
	image.Resize(8, 8);
	image.Blend(0, 0, 2, 2, 9, 9, 9, 255);

	image.Resize(8, 8);
	REQUIRE(At(image, 0, 0, 0) == 9);

	image.Resize(16, 16);
	REQUIRE_FALSE(AnyPixelSet(image));
}

TEST_CASE("text measures one advance per character less the trailing gap", "[overlay]") {
	REQUIRE(DebugText::Measure("", 1) == 0);
	REQUIRE(DebugText::Measure("A", 1) == DebugText::GLYPH_WIDTH);
	REQUIRE(DebugText::Measure("AB", 1) == DebugText::ADVANCE * 2 - 1);
	REQUIRE(DebugText::Measure("AB", 2) == (DebugText::ADVANCE * 2 - 1) * 2);
}

TEST_CASE("text draws pixels", "[overlay]") {
	OverlayImage image;
	image.Resize(64, 16);

	DebugText::Draw(image, 1, 1, "60 FPS", 255, 255, 255, 1);

	REQUIRE(image.IsDirty());
	REQUIRE(AnyPixelSet(image));
}

TEST_CASE("lowercase draws as uppercase rather than as nothing", "[overlay]") {
	OverlayImage lower;
	OverlayImage upper;
	lower.Resize(64, 16);
	upper.Resize(64, 16);

	DebugText::Draw(lower, 0, 0, "fps", 255, 255, 255, 1);
	DebugText::Draw(upper, 0, 0, "FPS", 255, 255, 255, 1);

	// The font has one case. Dropping the letters would be worse than shouting.
	for (size_t index = 0; index < lower.GetByteCount(); index++) {
		REQUIRE(lower.GetPixels()[index] == upper.GetPixels()[index]);
	}
}

TEST_CASE("an unknown character advances without drawing", "[overlay]") {
	OverlayImage image;
	image.Resize(64, 16);

	// A stray character must not shift a column of numbers, so it takes the
	// same width as a glyph and draws nothing.
	DebugText::Draw(image, 0, 0, "~", 255, 255, 255, 1);
	REQUIRE_FALSE(AnyPixelSet(image));

	REQUIRE(DebugText::Measure("~", 1) == DebugText::Measure("A", 1));
}

TEST_CASE("scale multiplies every glyph pixel", "[overlay]") {
	OverlayImage single;
	OverlayImage doubled;
	single.Resize(32, 16);
	doubled.Resize(32, 16);

	DebugText::Draw(single, 0, 0, "8", 255, 255, 255, 1);
	DebugText::Draw(doubled, 0, 0, "8", 255, 255, 255, 2);

	size_t singleSet = 0;
	size_t doubledSet = 0;
	for (size_t index = 3; index < single.GetByteCount(); index += 4) {
		singleSet += single.GetPixels()[index] != 0 ? 1 : 0;
		doubledSet += doubled.GetPixels()[index] != 0 ? 1 : 0;
	}

	REQUIRE(singleSet > 0);
	REQUIRE(doubledSet == singleSet * 4);
}
