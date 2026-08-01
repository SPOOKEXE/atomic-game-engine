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

TEST_CASE("the upload region is what was drawn, not the whole image", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	image.Fill(10, 20, 30, 40, 8, 10, 16, 208);

	// The image is the size of the window and the panels are a corner of it.
	// Sending all of it was the largest single cost in the frame.
	const auto region = image.UploadRegion();
	REQUIRE(region.X == 10);
	REQUIRE(region.Y == 20);
	REQUIRE(region.Width == 30);
	REQUIRE(region.Height == 40);
}

TEST_CASE("the upload region is the union of everything drawn", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	image.Fill(100, 100, 20, 20, 8, 10, 16, 208);
	image.Blend(10, 200, 30, 30, 255, 255, 255, 255);
	DebugText::Draw(image, 300, 5, "TEXT", 255, 255, 255, 2);

	// Every entry point has to record what it touched, including the text
	// rasteriser — which marks once per string rather than once per run.
	const auto region = image.UploadRegion();
	REQUIRE(region.X == 10);
	REQUIRE(region.Y == 5);
	REQUIRE(region.X + region.Width >= 300);
	REQUIRE(region.Y + region.Height == 230);
}

TEST_CASE("the upload region covers what a shrinking panel vacated", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	// A large panel, uploaded, then a small one in its corner. The GPU still
	// holds the large one's pixels, and only an upload covering them says they
	// are gone.
	image.Fill(0, 0, 200, 200, 8, 10, 16, 208);
	image.MarkUploaded();
	image.Clear();
	image.Fill(0, 0, 50, 50, 8, 10, 16, 208);

	const auto region = image.UploadRegion();
	REQUIRE(region.X == 0);
	REQUIRE(region.Y == 0);
	REQUIRE(region.Width == 200);
	REQUIRE(region.Height == 200);
}

TEST_CASE("a region survives frames in which nothing is drawn", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	image.Fill(0, 0, 200, 200, 8, 10, 16, 208);
	image.MarkUploaded();

	// The panels close. Nothing is uploaded on these frames, so the texture
	// keeps the large panel — and a Clear that forgot it on the second frame
	// would leave those pixels on screen the moment a smaller panel reopened.
	image.Clear();
	image.Clear();
	image.Clear();

	image.Fill(0, 0, 50, 50, 8, 10, 16, 208);

	const auto region = image.UploadRegion();
	REQUIRE(region.Width == 200);
	REQUIRE(region.Height == 200);
}

TEST_CASE("content outlives the upload that carried it", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);
	image.Fill(0, 0, 100, 100, 8, 10, 16, 208);

	REQUIRE(image.IsDirty());
	REQUIRE(image.HasContent());

	// The renderer takes the region and says so. The texture now holds the
	// picture, so there is nothing to send — but there is still something to
	// draw, which is what lets the panels be redrawn far less often than they
	// are presented.
	image.MarkUploaded();

	REQUIRE_FALSE(image.IsDirty());
	REQUIRE(image.HasContent());
	REQUIRE(image.UploadRegion().Width == 0);
}

TEST_CASE("an uploaded image redrawn in place asks for only the new region", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	image.Fill(0, 0, 100, 100, 8, 10, 16, 208);
	image.MarkUploaded();

	// A later frame redraws the same panel. The GPU already matches everywhere
	// else, so only what moved needs to travel.
	image.Clear();
	image.Fill(0, 0, 100, 100, 8, 10, 16, 208);

	const auto region = image.UploadRegion();
	REQUIRE(region.Width == 100);
	REQUIRE(region.Height == 100);
}

TEST_CASE("clearing an uploaded image still asks for the erase", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	image.Fill(0, 0, 100, 100, 8, 10, 16, 208);
	image.MarkUploaded();
	image.Clear();

	// Nothing is drawn any more, but the texture does not know that. Whether
	// the erase is worth sending is the renderer's call — it skips the pass
	// entirely — and the region has to be there for it either way.
	REQUIRE_FALSE(image.HasContent());
	REQUIRE(image.UploadRegion().Width == 100);
}

TEST_CASE("an untouched image asks for no upload", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);

	REQUIRE_FALSE(image.IsDirty());
	REQUIRE(image.UploadRegion().Width == 0);
	REQUIRE(image.UploadRegion().Height == 0);
}

TEST_CASE("resizing forgets the region with the texture", "[overlay]") {
	OverlayImage image;
	image.Resize(400, 300);
	image.Fill(0, 0, 200, 200, 8, 10, 16, 208);
	image.MarkUploaded();
	image.Clear();

	// A resize means a new texture, and nothing on it to correct.
	image.Resize(800, 600);

	REQUIRE_FALSE(image.IsDirty());
	REQUIRE(image.UploadRegion().Width == 0);
}

TEST_CASE("clipped and unclipped text draw the same pixels", "[overlay]") {
	// Text fully inside the image skips per-run clipping and writes straight
	// into the buffer; text that might cross an edge goes the long way. Two
	// paths drawing the same glyphs have to agree, or the panels change
	// appearance the moment one is dragged near an edge.
	OverlayImage wide;
	OverlayImage tight;
	wide.Resize(200, 40);
	// Exactly as wide as the text needs, so the whole-string bounds test fails
	// on the trailing advance and the clipped path runs.
	tight.Resize(DebugText::Measure("ABC 123", 2), 40);

	DebugText::Draw(wide, 0, 0, "ABC 123", 200, 100, 50, 2);
	DebugText::Draw(tight, 0, 0, "ABC 123", 200, 100, 50, 2);

	REQUIRE(tight.GetWidth() > 0);
	for (int y = 0; y < tight.GetHeight(); y++) {
		for (int x = 0; x < tight.GetWidth(); x++) {
			REQUIRE(At(tight, x, y, 0) == At(wide, x, y, 0));
			REQUIRE(At(tight, x, y, 3) == At(wide, x, y, 3));
		}
	}
}

TEST_CASE("text off the edge is clipped rather than corrupting memory", "[overlay]") {
	OverlayImage image;
	image.Resize(16, 16);

	// The unclipped write path has no bounds check at all, so the decision of
	// which path to take is load-bearing. Every one of these must take the slow
	// one.
	DebugText::Draw(image, -8, -3, "OFF THE EDGE", 255, 255, 255, 2);
	DebugText::Draw(image, 12, 12, "OFF THE EDGE", 255, 255, 255, 2);
	DebugText::Draw(image, 0, -20, "ABOVE", 255, 255, 255, 1);
	DebugText::Draw(image, 0, 40, "BELOW", 255, 255, 255, 1);
	DebugText::Draw(image, 500, 0, "RIGHT", 255, 255, 255, 1);

	// Reaching this line without a sanitiser complaint is most of the test.
	REQUIRE(image.GetByteCount() == 16u * 16u * OverlayImage::BYTES_PER_PIXEL);
}

TEST_CASE("a run of spaces draws nothing and still advances", "[overlay]") {
	OverlayImage image;
	image.Resize(64, 16);

	// Rows are padded to a fixed column width, so most of a line is spaces. They
	// are skipped without walking their bits — but the cursor still has to move,
	// or every column after the first gap lands in the wrong place.
	DebugText::Draw(image, 0, 0, "   A", 255, 255, 255, 1);

	REQUIRE(image.IsDirty());

	// Nothing in the first three advances.
	for (int x = 0; x < 3 * DebugText::ADVANCE; x++) {
		for (int y = 0; y < 5; y++) {
			REQUIRE(At(image, x, y, 3) == 0);
		}
	}

	// And the A is where it would have been without the skip.
	REQUIRE(At(image, 3 * DebugText::ADVANCE, 0, 3) == 255);
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
