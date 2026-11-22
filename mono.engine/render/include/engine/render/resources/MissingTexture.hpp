#pragma once

// arch-waiver public-header: forward renderer API. Asset hosts use this
// complete missing-texture fallback contract.

// The picture a drawable gets when it names a texture that is not here.
//
// **A different question from the one `DefaultTexture` answers, and keeping
// them apart is the whole point.** That header draws `Material = None` - a part
// nobody textured, which is a finished state and looks like white plastic
// because that is what it is. This draws a part that *asked* for a sheet the
// renderer does not hold, which is not a finished state at all. Rendering both
// as the default plastic makes an author's typo look like a deliberate
// material, and the two are indistinguishable on screen right up until somebody
// ships one.
//
// It is the same distinction `scene::KeepLoaded` draws for geometry: no mesh
// named draws the default cube, a mesh named and absent draws nothing. Here the
// second case has something to draw, because a surface must sample *something*
// and the useful choice is a picture that says what happened.
//
// ## Purple and black, because the convention is older than this engine
//
// Half the industry's missing texture is a magenta checkerboard, and an author
// who has touched any engine reads it before they have read a log line. That is
// the entire argument - it is not a nicer colour, it is the one that already
// means this. Two properties earn it beyond familiarity:
//
//   - **No author picks it.** Full-saturation magenta against black is outside
//     what anything in a real scene is made of, so it can never be mistaken for
//     content that loaded correctly.
//   - **The checks make it legible at any distance.** A flat colour at fifty
//     metres is a coloured part; a pattern that keeps alternating still reads as
//     a marker, and the tiling tells you the UVs are live.
//
// **Generated rather than compiled in.** `DefaultTexture` is a photograph and
// has provenance to preserve; this is two colours and a parity test, and sixteen
// lines of arithmetic beat sixteen kilobytes of blob that nobody can review.
//
// **RGBA8 where the default is R8**, because the colour is the message. The
// default is a greyscale sheet that a base colour multiplies into whatever the
// author asked for; this one has to arrive on screen as itself, so the caller
// binding it also neutralises the base colour rather than letting a red part
// turn the marker into a dark pattern that reads as intentional.
//
// @tier L12 · client

#include <engine/assets/Texture.hpp>

#include <cstdint>

namespace engine::render {

	// How wide the marker is, in pixels. Square.
	inline constexpr uint32_t MISSING_TEXTURE_SIDE = 64;

	// How wide one check is. Divides the side, so the grid is exact and the
	// tile repeats without a seam.
	inline constexpr uint32_t MISSING_TEXTURE_CHECK = 8;

	// The marker's pixels, ready to upload.
	//
	// Built on the first call and kept. A function-local static, so it is built
	// after the process is running rather than during static initialisation -
	// the same shape `DefaultTexture` uses and for the same reason.
	//
	// @return A 64x64 `RGBA8` checkerboard. Valid on every call.
	// @since v0.12
	const assets::TextureData &MissingTexture();
}
