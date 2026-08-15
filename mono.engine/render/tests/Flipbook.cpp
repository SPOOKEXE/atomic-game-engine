// Which cell of an animation sheet is showing.
//
// **`Renderer.hpp` is the only header in this module with no suite** - it needs
// a GPU and `AGENTS.md` says not to close that gap with a mock. This is not that
// header: it is arithmetic over four numbers, three passes read the same answer
// from it, and the failure it prevents is a GIF that plays in the wrong order or
// stops after nineteen days.

#include <engine/render/Flipbook.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.flipbook")

using Catch::Approx;
using engine::render::FlipbookCell;
using engine::render::FlipbookCellAt;
using engine::render::FlipbookFrameAt;

TEST_CASE("a still image is the identity", "[render][flipbook]") {
	// **The case every caller applies unconditionally**, which is what lets the
	// shader have no branch: a rectangle, a glyph and every ordinary texture
	// come through here and must come out unchanged.
	const FlipbookCell still = FlipbookCellAt(0, 0, 0.0f, 12.5);
	CHECK(still.Scale == 1.0f);
	CHECK(still.OffsetU == 0.0f);
	CHECK(still.OffsetV == 0.0f);

	// A one-cell grid is a still too, and a sheet with no frames in it is one
	// whatever its grid says - a texture whose side survived and whose count did
	// not is corrupt, and showing frame zero is the safe reading.
	CHECK(FlipbookCellAt(1, 1, 12.0f, 3.0).Scale == 1.0f);
	CHECK(FlipbookCellAt(4, 0, 12.0f, 3.0).Scale == 1.0f);
}

TEST_CASE("frames advance on the clock and wrap on the frame count", "[render][flipbook]") {
	// **Wrapped on the count, not on the grid.** A 24-frame GIF lands in an 8x8
	// with forty cells empty, and a player that walked all sixty-four would
	// spend five eighths of every loop showing nothing - which reads as a GIF
	// with a long pause in it rather than as a bad modulo.
	CHECK(FlipbookFrameAt(24, 10.0f, 0.0) == 0);
	CHECK(FlipbookFrameAt(24, 10.0f, 0.05) == 0);
	CHECK(FlipbookFrameAt(24, 10.0f, 0.15) == 1);
	CHECK(FlipbookFrameAt(24, 10.0f, 2.35) == 23);

	// One full loop later, back to the start.
	CHECK(FlipbookFrameAt(24, 10.0f, 2.45) == 0);
}

TEST_CASE("a sheet with no rate still plays", "[render][flipbook]") {
	// A hand-drawn sheet states no rate, and a GIF whose delays were all zero
	// decodes to none either. Holding frame zero for ever is the one answer that
	// is certainly wrong, so both play at the rate `bake::ReadGif` already reads
	// a zero delay as.
	CHECK(FlipbookFrameAt(4, 0.0f, 0.0) == 0);
	CHECK(FlipbookFrameAt(4, 0.0f, 0.25) == 2);
}

TEST_CASE("cells are laid out row-major from the top left", "[render][flipbook]") {
	// **The order `bake::ReadGif` writes them in.** Reading them column-major
	// would play a GIF in an order nothing produced, which looks like a shuffled
	// animation rather than like two axes swapped - so it is the kind of bug
	// somebody would chase in the decoder.
	const FlipbookCell first = FlipbookCellAt(4, 16, 10.0f, 0.0);
	CHECK(first.Scale == Approx(0.25f));
	CHECK(first.OffsetU == Approx(0.0f));
	CHECK(first.OffsetV == Approx(0.0f));

	// Frame 1 is one cell right, still on the top row.
	const FlipbookCell second = FlipbookCellAt(4, 16, 10.0f, 0.15);
	CHECK(second.OffsetU == Approx(0.25f));
	CHECK(second.OffsetV == Approx(0.0f));

	// Frame 4 wraps to the start of the second row.
	const FlipbookCell fifth = FlipbookCellAt(4, 16, 10.0f, 0.45);
	CHECK(fifth.OffsetU == Approx(0.0f));
	CHECK(fifth.OffsetV == Approx(0.25f));

	// The last cell of a full grid is the bottom right corner.
	const FlipbookCell last = FlipbookCellAt(4, 16, 10.0f, 1.55);
	CHECK(last.OffsetU == Approx(0.75f));
	CHECK(last.OffsetV == Approx(0.75f));
}

TEST_CASE("a cell never runs off the sheet", "[render][flipbook]") {
	// The property the shader depends on: scale plus offset is at most one on
	// both axes, so a sampled coordinate stays inside the texture. A cell that
	// ran off would smear the opposite edge in, because the sampler repeats.
	for (uint8_t side : {2, 4, 8}) {
		const auto frames = static_cast<uint8_t>(side * side);
		for (int step = 0; step < 200; step++) {
			const FlipbookCell cell = FlipbookCellAt(side, frames, 12.0f, step * 0.037);
			CHECK(cell.OffsetU + cell.Scale <= 1.0f + 1e-5f);
			CHECK(cell.OffsetV + cell.Scale <= 1.0f + 1e-5f);
			CHECK(cell.OffsetU >= 0.0f);
			CHECK(cell.OffsetV >= 0.0f);
		}
	}
}

TEST_CASE("a long session does not stop animating", "[render][flipbook]") {
	// **The reason the modulo is taken in `double`.** At ten frames a second a
	// 32-bit frame count wraps after about seven years and a `float` stops
	// resolving whole numbers after about nineteen days - at which point an
	// animation visibly stutters and then holds. A session that long is a server
	// rather than a play session, and a server drawing nothing still runs this.
	const double nineteenDays = 19.0 * 24.0 * 60.0 * 60.0;
	const uint32_t before = FlipbookFrameAt(24, 10.0f, nineteenDays);
	const uint32_t after = FlipbookFrameAt(24, 10.0f, nineteenDays + 0.1);
	CHECK(before < 24);
	CHECK(after < 24);
	CHECK(before != after);
}

TEST_CASE("time before the clock started holds the first frame", "[render][flipbook]") {
	// A modulo of a negative is implementation-defined in sign in C++, which
	// would show as an animation running backwards on one platform and not on
	// another - the worst kind of difference to be told about.
	CHECK(FlipbookFrameAt(24, 10.0f, -5.0) == 0);
	CHECK(FlipbookCellAt(4, 16, 10.0f, -5.0).OffsetU == 0.0f);
}
