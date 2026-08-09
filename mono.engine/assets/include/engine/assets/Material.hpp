#pragma once

// What an `AssetKind::Material`'s bytes are.
//
// **`AssetKind::Material` named a kind and nothing wrote one**, which
// `ROADMAP.md` v0.10 lists in as many words: `Submesh::Material` was what a
// source file called a run, `Submesh::Texture` was an asset that existed, and
// there was nothing in between binding the two. So a part's material was an
// *enum* — `Plastic`, `Wood`, `Metal` — seventeen names a renderer could
// plausibly be asked to draw and did not draw differently, because a name is not
// a texture. This is the format that closes that gap: a material is an asset
// like a mesh or a texture, it is published and fetched like one, and what it
// carries is which textures to sample.
//
// **One map, and the rest are absent rather than declared.** `ColourMap` is what
// the opaque pass samples today; a `MetalnessMap` field nothing read would be
// half a feature somebody would reasonably assume worked — the same rule
// `scene::SurfaceAppearance` already states about exactly these fields.
// `ROADMAP.md` v0.11 is where the G-buffer arrives and where the other maps get
// a reader. The fetched content already carries them as ordinary textures beside
// the material, so what is missing is a field and a pass rather than the pixels.
//
// **A name, not a hash, and not a handle.** Rule 4: a material references its
// texture across a save file, a manifest and a wire, so the reference is the
// string a publisher wrote. `assetc` is what turns `Bricks075A_Color.png` into
// `materials/ambientcg/Bricks075A_Color.atex`, and it does it through the same
// `BakedName` a model's texture reference goes through — one spelling of the
// rule, because two would be a material resolving to nothing.
//
// **A runtime does not author one.** Turning a `.mat` into this is a publishing
// step, exactly as turning a PNG into a `.atex` is, and for the same reason
// `Texture.hpp` gives: the origin does the work once and every client does none.
//
// @tier L8 · shared

#include <engine/core/Bytes.hpp>

#include <cstdint>
#include <string>

namespace engine::assets {

	// A material, as the file holds it.
	//
	// @since v0.10
	struct MaterialData {
		// The texture sampled for base colour, as a published asset name.
		//
		// **May be empty, and an empty one is a real state rather than a
		// malformed file.** A material that names no texture is drawn with the
		// engine's own default — `render::DefaultTexture` — which is what makes
		// "no texture to render" a thing an author can say. A file refused for
		// having no colour map would make the null case unrepresentable and push
		// it back onto the absence of the material itself, which is a different
		// fact: no material at all is `Material = None`, and a material with no
		// colour map is one somebody authored and has not textured yet.
		std::string ColourMap;

		// The other four maps a published material carries.
		//
		// **Published since v0.10 and read by nothing until now.** `ROADMAP.md`
		// said these arrive "when there is a pass that samples them, which is
		// v0.11's G-buffer" — that pass exists, so they arrive. All 291 seeded
		// materials already ship the full set at 1K; what was missing was
		// anywhere to put the names.
		//
		// **Each may be empty, for `ColourMap`'s reason.** A material with no
		// normal map is one somebody authored flat, not a malformed file, and
		// the shader falls back to the geometric normal. Refusing a material for
		// an absent map would make the ordinary case unrepresentable.
		//
		// **Names rather than packed channels.** Roughness, occlusion and height
		// are single-channel and an engine that cared most about bandwidth would
		// pack the three into one RGB texture. The sources do not: ambientCG,
		// Poly Haven and cgbookcase each publish them separately, so packing
		// would be a bake step that has to run before anything can be looked at,
		// and a mismatch between packed and unpacked would be invisible until it
		// rendered. Separate names now; packing is a bake decision that can be
		// made later without changing what a material *is*.
		//@{
		std::string NormalMap;
		std::string RoughnessMap;
		std::string OcclusionMap;
		std::string HeightMap;

		// What this surface emits regardless of what lights it.
		//
		// **The one map with no CC0 source behind it.** ambientCG, Poly Haven
		// and cgbookcase publish the other four for nearly every material and an
		// emissive map for almost none — it is authored per-asset, because what
		// glows is a decision about the object rather than a property of the
		// substance. So this is carried and sampled with a hand-authored test
		// material rather than with the seeded set.
		std::string EmissiveMap;
		//@}

		// Whether this describes a material at all.
		//
		// @return `true` always, today. **A function rather than nothing**, so
		//         the shape matches `MeshData` and `TextureData` and a caller
		//         checking one checks all three the same way — the field that
		//         makes this answer `false` arrives with the second map.
		bool IsValid() const {
			return true;
		}
	};

	// Reading and writing the material format.
	//
	// Static, because a material has no state — `Texture`'s shape and its
	// reason.
	//
	// @since v0.10
	class Material {
	  public:
		// "AMT1", and `Texture::MAGIC`'s reason: a file that is not this format
		// has to fail as "not this format" rather than as a plausible length.
		static constexpr uint32_t MAGIC = 0x31544D41;

		// The version. Bumped when the layout changes, never reused.
		//
		// **3 adds emissive, 2 added the other four, and 1 still reads.** A version 1 file is a
		// colour map and nothing else, which is exactly a material whose other
		// four names are empty — so the older format is not a special case to
		// translate, it is the newer one with four absent fields. That is what
		// makes reading it a branch on how many strings to expect rather than a
		// second parser.
		static constexpr uint16_t VERSION = 3;

		// The longest asset name this will read.
		//
		// **A bound on what a corrupt header can make a reader allocate**, the
		// same reasoning `Texture::MAXIMUM_DIMENSION` carries. A manifest name is
		// a path a publisher wrote; a kilobyte is far past any real one and far
		// below anything that would matter.
		static constexpr uint32_t MAXIMUM_NAME = 1024;

		// Writes a material.
		//
		// @param writer Where the bytes go.
		// @param data   The material.
		// @return `false` when `data` is not a valid material, or names a
		//         texture longer than `MAXIMUM_NAME`.
		static bool Write(core::ByteWriter &writer, const MaterialData &data);

		// Reads a material, refusing anything that is not one.
		//
		// Refuses a wrong magic, an unknown version and a name past
		// `MAXIMUM_NAME`.
		//
		// @param reader The bytes to parse.
		// @param out    Filled in on success, left alone otherwise — so a caller
		//               reusing one cannot act on a mixture of the last good
		//               material and a bad one.
		// @return `false` on anything malformed. Drop it and count it.
		static bool Read(core::ByteReader &reader, MaterialData &out);
	};
}
