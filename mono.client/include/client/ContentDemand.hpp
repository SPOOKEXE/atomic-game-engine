#pragma once

// Which textures a world is actually asking for.
//
// **Requesting by kind stopped working the moment a store got large, and the
// failure was silent in the way that costs a day.** `Client::PumpContent` asked
// for every asset of every kind the catalogue held, which was right while a
// store was a demo's worth of content and is not right at 1,637 textures: an
// uncompressed 1K sheet is four megabytes and a 2K one is sixteen, so
// `render::TextureTable::MAXIMUM_BYTES` — 512 MB, a bound on device memory
// reachable from content — is spent after about a hundred and forty of them.
//
// The rest were refused, in arrival order, which is *manifest* order and has
// nothing to do with what the scene names. So a part naming a texture drew
// untextured, and the only trace was 1,463 identical warnings in a log nobody
// reads while a scene looks wrong. The table was doing exactly its job.
//
// **So a texture is fetched because something names it.** That is the shape the
// engine wanted anyway — `ROADMAP.md` v0.11's streaming is this question asked
// per view rather than per world — and it is what makes a store with a hundred
// thousand assets behave the same as one with ten.
//
// ## What is still requested by kind, and why that is not an oversight
//
// **Meshes, materials and audio.** Each is small in a way a texture is not: a
// `.amat` is a magic, a version and one name; a mesh is geometry rather than
// sheets; a sound is decoded once and shared. More to the point, a *material*
// has to arrive before anything can know which texture it names — a demand pass
// that waited for materials to be demanded would deadlock on itself.
//
// The same ceiling will arrive for meshes on a big enough store, and it will
// look exactly like this did. Said here rather than discovered again.
//
// @tier L13 · client

#include <engine/core/Name.hpp>

#include <vector>

namespace engine::ecs {
	class Store;
}

namespace client {

	// Appends every texture name the world currently mentions.
	//
	// **Every place a texture can be named, and the list is the point.** Missing
	// one is a class of asset that never loads while everything else does, which
	// reads as that asset being broken rather than as a name nobody asked for:
	//
	//   * `scene::SurfaceAppearance::ColourMap` — a part's texture, and the
	//     field a `Material` instance resolves into, so materials are covered by
	//     this row rather than by one of their own.
	//   * `gui::Picture::Image` — an `ImageLabel`, which drew its missing-image
	//     marker for exactly this reason.
	//   * `effects::ParticleEmitter::Texture`, and a `Beam`'s and a `Trail`'s.
	//
	// **A mesh's own sheets are not here**, and cannot be: `Submesh::Texture`
	// lives in the mesh file, so the name is not known until the mesh has
	// arrived. `Client::PumpContent` asks for those when it registers the mesh,
	// which is the one point where they are readable.
	//
	// **A mutable store for a read-only walk**, because `Store::Each` is not
	// `const` — it builds a query. Taking a `const&` here would mean a
	// `const_cast` inside, which hides the same fact one layer further from
	// anybody who has to trust it.
	//
	// @param store The world to read.
	// @param out   Appended to. Duplicates are left in — the caller is diffing
	//              against what it has already asked for, and de-duplicating
	//              twice is one place too many.
	void CollectWantedTextures(engine::ecs::Store &store, std::vector<engine::core::Name> &out);
}
