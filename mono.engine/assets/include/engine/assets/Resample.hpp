#pragma once

// Box-filtering an `assets::TextureData`, and the mip chain built out of it.
//
// **Arithmetic over the format, which is why it sits beside the format rather
// than above it.** This was `bake::ResizeImage` at L9 until v0.15, and being one
// tier up meant `assets` itself could not use it: `MakeBuiltin`'s checker,
// `render::DefaultTexture` and `render::MissingTexture` are all pixels generated
// below the importers, so all three uploaded at a single level and shimmered at
// distance. Nothing here reads a foreign file or touches a vendor — it is a
// weighted average over bytes this module already defines — so the tier it was
// living at was an accident of where mip chains were first needed.
//
// `bake` still owns everything that turns *somebody else's* file into one of
// these. This owns what happens to one afterwards.
//
// @tier L8 · shared

#include <engine/assets/Texture.hpp>

#include <cstdint>

namespace engine::assets {

	// Resamples an image with a box filter. Upscaling duplicates source pixels.
	//
	// **The mip chain is not carried across**, unlike the flipbook triple: a
	// level's size is derived from the base dimensions, so keeping the old levels
	// past a resize would leave level one larger than level zero. Rebuild it with
	// `BuildMipChain` after the last resize.
	//
	// @param source The image to resample.
	// @param width  The target width. Zero refuses.
	// @param height The target height. Zero refuses.
	// @param out    Filled on success. May not alias `source`.
	// @return `false` for a zero or over-large target, or an invalid source.
	// @since v0.15
	bool ResizeImage(const TextureData &source, uint32_t width, uint32_t height, TextureData &out);

	// How many levels a chain over this image may honestly have.
	//
	// A still image gets the full `MipLevelCount`. **A flipbook sheet gets fewer,
	// and that is the whole of the decision** — see `BuildMipChain`.
	//
	// @param image The image, chain or no chain.
	// @return The count, level zero included. Zero for an invalid image, and one
	//         for an image that can carry no chain at all.
	// @since v0.15
	uint32_t MipChainLevels(const TextureData &image);

	// Builds an image's mip chain in place, halving with the box filter.
	//
	// Each level is filtered from the one above rather than from the base, which
	// is what a sampler interpolating between two adjacent levels expects.
	//
	// **A flipbook sheet stops early rather than being padded or refused.**
	// Halving a grid of frames is safe only while every destination pixel still
	// falls inside one cell; one level past that, a pixel averages two frames and
	// the sheet shows a ghost of the next frame at distance — which reads as the
	// flipbook's cell arithmetic being wrong rather than as a chain one level too
	// long. So the chain ends at the last level whose cells are still an exact
	// halving, which is the level where each frame is one pixel. Padding the
	// cells with gutters would change what a flipbook *is* — every consumer
	// divides the sheet by `FlipbookSide` — and refusing the texture outright
	// would throw away an image that was perfectly good without a chain.
	//
	// @param[in,out] image The image. Any existing chain is replaced, so running
	//                a pipeline twice produces the same bytes.
	// @return `false` for an image that is not one.
	// @since v0.15
	bool BuildMipChain(TextureData &image);
}
