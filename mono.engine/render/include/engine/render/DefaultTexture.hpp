#pragma once

// arch-waiver public-header: forward renderer API. Renderer hosts share this
// complete default-texture contract.

// The texture a drawable gets when nothing has said what it is made of.
//
// **The gap this closes is that "no texture" had no picture.** A part with no
// material sampled `FallbackTexture` - one white texel, created so an unbound
// sampler binding is not uninitialised device memory - and one white texel is a
// stand-in for a *binding*, not a material. So every untextured part in every
// scene was flat white, and the seventeen-member `Material` enum that was
// supposed to say otherwise said nothing a renderer could sample.
//
// This is what `Material = None` draws as, and it is a real texture: the colour
// map of ambientCG's **Plastic 013 A**, CC0, box-filtered to a 64-pixel tile.
// `THIRD_PARTY_NOTICES.md` carries the entry.
//
// ## Why it is compiled in rather than fetched
//
// Everything else the renderer samples arrives through `delivery::AssetClient`,
// and the default cannot: it is what a part draws with *before* any content has
// streamed, on a machine with no content store, and on the frame a fetch fails.
// A default that had to be fetched would be absent in precisely the cases it
// exists for.
//
// **Four kilobytes, and that is what the format choice buys.** The source is a
// near-neutral 233–238 across the whole sheet - a plastic's character is in its
// normal and roughness maps, not its colour - so the tile is one channel, and
// `TextureTable::Add` already widens `R8` to the pipeline's format, so nothing
// here repeats that. The 237/234/233 tint the source carries is dropped with the
// other two channels, deliberately: this is the *white* default, and a warm cast
// on every untextured part in the engine is a decision no author made.
//
// **And it is lifted so the brightest texel is white - 250 to 255 rather than
// 233 to 238.** The default multiplies whatever base colour an author set, so
// shipping the sheet at its photographed exposure would darken every untextured
// part in every existing scene by eight per cent, which reads as a lighting
// regression with no line of code to blame it on. The offset keeps the grain and
// moves the sheet to where "white" means white.
//
// @tier L12 · client

#include <engine/assets/Texture.hpp>

namespace engine::render {

	// The default's pixels, ready to upload.
	//
	// Built from the compiled-in tile on the first call and kept. A
	// function-local static, so it is built after the process is running rather
	// than during static initialisation.
	//
	// @return A 64x64 `R8` image. Valid on every call.
	// @since v0.10
	const assets::TextureData &DefaultTexture();
}
