#pragma once

// arch-waiver public-header: forward renderer API. Animation and rendering
// hosts use this complete flipbook contract.

// Which cell of an animation sheet is showing, and where it is.
//
// **A GIF has been an ordinary texture since v0.10 and nothing played it.**
// `bake::ReadGif` lays the frames out as a square power-of-two grid and
// `assets::TextureData` carries the grid, the frame count and the rate - so the
// pixels, the layout and the timing all arrived, and every pass sampled cell
// zero. A `.gif` on a part or an `ImageLabel` was a still of its first frame,
// which reads as the decoder having dropped the animation rather than as
// nothing advancing it.
//
// This is the half that advances it, and it is arithmetic rather than a system:
// three paths need the same answer: geometry for a part, the interface
// pass for an `ImageLabel`, and the studio's imgui painter for the same label in
// a viewport panel - and three copies of a modulo would be three chances to
// disagree about whether a sheet loops on the frame count or the cell count.
//
// **The particle path is deliberately not a caller.** An emitter's flipbook is
// per *particle*: `OneShot` stretches the sheet over a particle's own lifetime
// and `Random` holds one cell for the whole of it, so the cell is a function of
// that particle's age rather than of the clock. `effects::FlipbookLayout` is
// that, and it stays there.
//
// **No clock.** `seconds` is passed in, for the standing rule `assets::Grant`
// and `cdn::Service::Pump` both keep: a module that read the time would hold a
// notion of "now" of its own to drift, and a test could not pin it.
//
// @tier L12 · client

#include <cstdint>

namespace engine::render {

	// Where a cell sits in its sheet, as a transform on a 0-to-1 texture
	// coordinate.
	//
	// **A scale and an offset rather than a cell index**, because that is what a
	// shader wants and it makes the still case free: a texture that is not a
	// sheet gets the identity, so every path applies the transform
	// unconditionally and no pass needs a branch per fragment.
	//
	// @since v0.10
	struct FlipbookCell {
		// Multiplied into the coordinate. `1 / side` for a sheet, 1 for a still.
		float Scale = 1.0f;

		// Added after the scale.
		//@{
		float OffsetU = 0.0f;
		float OffsetV = 0.0f;
		//@}
	};

	// Which frame is showing at `seconds`.
	//
	// **Wrapped on the frame count and not on the grid.** A 24-frame GIF lands
	// in an 8x8 with forty cells empty - `bake/Gif.cpp` says why the grid is
	// square and a power of two - and a player that walked all sixty-four would
	// spend five eighths of every loop showing nothing.
	//
	// @param frames  How many cells hold a frame. Zero is a still image.
	// @param rate    Frames a second the source was authored at. Zero means the
	//                source did not say, and is taken as `DEFAULT_RATE`: a
	//                sheet drawn by hand states nothing, and holding frame zero
	//                for ever is the one answer that is certainly wrong.
	// @param seconds How long the animation has been running.
	// @return The frame index, always below `frames`, or zero for a still.
	uint32_t FlipbookFrameAt(uint8_t frames, float rate, double seconds);

	// Where that frame sits in the sheet.
	//
	// @param side    The grid's side. Zero or one is a still image.
	// @param frames  How many cells hold a frame.
	// @param rate    Frames a second, or zero for `DEFAULT_RATE`.
	// @param seconds How long the animation has been running.
	// @return The transform. The identity for anything that is not a sheet.
	FlipbookCell FlipbookCellAt(uint8_t side, uint8_t frames, float rate, double seconds);

	// What a sheet that states no rate is played at.
	//
	// Ten a second, which is what `bake::ReadGif` already treats a zero or
	// one-hundredth delay as - the de-facto browser rule since Netscape. Spelled
	// here as well so a hand-drawn sheet and a GIF with no delays play the same.
	inline constexpr float DEFAULT_RATE = 10.0f;
}
