#pragma once

// Which content a world is actually asking for.
//
// **Requesting by kind stopped working the moment a store got large, and it
// failed twice in two different ways.**
//
// The first was textures. `render::TextureTable::MAXIMUM_BYTES` is 512 MB - a
// bound on device memory reachable from content - and an uncompressed 1K sheet
// is four megabytes, so a store of 1,637 of them spends the ceiling after about
// a hundred and forty. The rest were refused in *manifest* order, which has
// nothing to do with what a scene names, so the texture somebody asked for was
// usually among the refused and the only trace was 1,463 identical warnings in a
// log nobody reads.
//
// The second was everything else, and it is worse because it is not a ceiling
// but a stall. **The unit that travels is a bundle**, not an asset - `delivery/
// Client.hpp` says so, and it is the right design: per-asset requests would be
// thousands of round trips. It means asking for one mesh fetches the bundle
// carrying it. Asking for *every* mesh and material by kind therefore asks for
// essentially every bundle in the store, and `AssetClient::Pump` resolves,
// fetches, verifies and decompresses all of it **synchronously**, because the
// same header forbids a background thread: a completion that arrived at a moment
// scheduling chose would be a desync. On this repository's own store that is
// 6.9 GB through one function on the frame the editor opens.
//
// So: **nothing is fetched by kind. A world names it or it is not fetched.**
// That is the shape `ROADMAP.md` v0.11's streaming wants anyway - this is the
// same question asked per world rather than per view - and it is what makes a
// store with a hundred thousand assets behave like one with ten.
//
// ## Asynchrony, and what it can mean here
//
// It cannot mean a thread. What it means is that the *asking* is spread: a
// caller issues a bounded number of new requests per pump, so a scene naming
// five hundred assets becomes five hundred assets arriving over several seconds
// rather than one frame that never ends. The collection below is idempotent, so
// a caller re-runs it each pump and picks up where it left off with no queue of
// its own to keep in step.
//
// @tier L13 · client

#include <engine/core/Name.hpp>

#include <vector>

namespace engine::ecs {
	class Store;
}

namespace client {

	// Appends every content name the world currently mentions.
	//
	// **Every place content can be named, and the list is the point.** Missing
	// one is a class of asset that never loads while everything else does, which
	// reads as that asset being broken rather than as a name nobody asked for:
	//
	//   * `scene::Visual::Mesh` - a `MeshPart`'s geometry.
	//   * `scene::SurfaceAppearance::ColourMap` - a part's texture, and the
	//     field a `Material` instance resolves into, so a material's *sheet* is
	//     covered by this row rather than by one of its own.
	//   * `scene::MaterialRef::Asset` - the material itself.
	//   * `scene::Sound::SoundId` - and audio is here rather than by kind
	//     because one of this store's own files is a six-minute MP3 that costs a
	//     decode on arrival.
	//   * `gui::Picture::Image` - an `ImageLabel`.
	//   * `effects::ParticleEmitter::Texture`, and a `Beam`'s and a `Trail`'s.
	//
	// **A mesh's own sheets are not here**, and cannot be: `Submesh::Texture`
	// lives inside the mesh file, so the name is not known until the mesh has
	// arrived. A caller asks for those when it registers the mesh, which is the
	// one point where they are readable.
	//
	// **A material's colour map is not here either, and for a different
	// reason.** It is named inside the `.amat`, and it reaches a part through
	// `scene::ResolveMaterials`, which writes it into `SurfaceAppearance::
	// ColourMap` - a field this already reads. So the indirection needs no
	// special case: the next pump asks for the sheets of the materials something
	// is actually made of. Asking on arrival instead is requesting by kind again
	// one step later, which is how it was written the first time.
	//
	// **A mutable store for a read-only walk**, because `Store::Each` is not
	// `const` - it builds a query. Taking a `const&` here would mean a
	// `const_cast` inside, which hides the same fact one layer further from
	// anybody who has to trust it.
	//
	// @param store The world to read.
	// @param out   Appended to. Duplicates are left in - the caller is diffing
	//              against what it has already asked for, and de-duplicating
	//              twice is one place too many.
	void CollectWantedContent(engine::ecs::Store &store, std::vector<engine::core::Name> &out);
}
