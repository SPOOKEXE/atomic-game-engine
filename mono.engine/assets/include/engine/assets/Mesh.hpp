#pragma once

// What an `AssetKind::Mesh`'s bytes are.
//
// `AssetKind.hpp` said in as many words that `Mesh` meant "the mesh pipeline's,
// when there is one", and that the import and cooking work was `ROADMAP.md`
// v0.9's and would land beside it rather than under it. This is that file, and
// it is `Texture.hpp`'s argument applied to geometry: **a runtime does not
// import.** Turning a glTF, an OBJ or a PMX into this is a publishing step, so
// the client's whole cost is a bounds check and an upload.
//
// That division is not tidiness. An importer is a parser for somebody else's
// format, which means it is the largest attack surface a content pipeline has;
// keeping it out of the shipped binary means a malformed model can at worst
// break a build. It also means the vertex layout on disk is already the vertex
// layout the GPU wants, so streaming a mesh in costs no conversion pass on the
// frame it arrives.
//
// **Uncompressed here, compressed in transit** — `delivery::GroupCodec` runs
// zstd over whatever a group holds, exactly as it does for a texture sheet, so
// the interleaved float layout costs its real size on disk and its compressed
// size on the wire.
//
// @tier L8 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::assets {

	// One vertex, in the layout a vertex buffer wants.
	//
	// **Plain float arrays rather than `core::Vector3`**, and the difference is
	// the point: this is a device layout that happens to be readable rather than
	// a value type that happens to be uploadable. A `Vector3` gaining a fourth
	// component, an alignment attribute or a constructor would silently change
	// what a published mesh means, and nothing would say so. `render` binds
	// this struct directly as its vertex layout rather than keeping a copy,
	// which is what stops a published mesh and a built-in disagreeing about
	// what a vertex is.
	//
	// Interleaved rather than one stream per attribute. A draw reads all three
	// of these per vertex, so splitting them costs three cache lines where one
	// would do; the streams only pay off for a depth-only pass wide enough to
	// care, which this pipeline is not.
	//
	// @since v0.9
	struct MeshVertex {
		// Object-space position.
		float Position[3];

		// Object-space normal. Not required to be unit length by the format —
		// see `MeshData::IsValid` — because an importer that welds vertices
		// produces sums that a normalise step has yet to touch, and a format
		// that refused them would make the normalise mandatory before the file
		// could even be written back out for inspection.
		float Normal[3];

		// Texture coordinate, with `v` running down the image the way a
		// texture's row zero does.
		//
		// **The flip is the importer's, once, and never the shader's.** glTF
		// and PMX disagree with OBJ about which way `v` runs, and a renderer
		// that flipped would flip every format including the ones already
		// right. Baking the convention in here is what makes a sampled texture
		// look the same whatever it was imported from.
		float TexCoord[2];
	};

	// One run of indices drawn with one material.
	//
	// **A mesh is not one material and pretending otherwise loses the model.**
	// The PMX models this was built against carry a dozen or more — face, hair,
	// eyes, each clothing piece — and a format with a single material per file
	// would either draw them all with one texture or force the importer to emit
	// a dozen files and lose the fact that they are one thing.
	//
	// @since v0.9
	struct Submesh {
		// Where this run starts, as an index into `MeshData::Indices`.
		uint32_t FirstIndex = 0;

		// How many indices it covers. Always a multiple of three.
		uint32_t IndexCount = 0;

		// Which material, by name.
		//
		// **A `std::string` and deliberately not a `core::Name`**, which is the
		// one place this format departs from `AGENTS.md` rule 4's usual answer.
		// Interning takes a process-wide mutex and grows a registry that is
		// never emptied, and every byte here arrives from an origin anybody may
		// run — so a mesh naming ten thousand distinct materials would be an
		// unbounded allocation in a shared table, reachable from content.
		// `delivery::Asset::Name` is a `std::string` for the same reason. The
		// consumer interns when it *registers* the mesh, which is the point
		// where the name has already been accepted.
		//
		// Empty means the consumer's default material.
		std::string Material;

		// Which texture this run samples, as a published asset name.
		//
		// **Two fields rather than one, and the second is the load-bearing
		// one.** `Material` is what the source file called this run — `hair`,
		// `face_02` — and is informational: there is no material format in this
		// engine, `AssetKind::Material` names a kind nothing writes, and a name
		// out of somebody's modelling package resolves to nothing. This names
		// an asset that *exists*, which is what an importer can actually
		// produce and what a renderer can actually look up.
		//
		// Collapsing them was the tempting mistake. A model's material names
		// are not unique across a library — two characters both have a `hair` —
		// so a single field would either lose the model's own vocabulary or
		// make two unrelated meshes fight over one texture. When a material
		// format arrives this becomes what that material references and the
		// field above becomes what it is called.
		//
		// Empty means the run samples nothing and draws with its tint, which is
		// the ordinary case for an untextured model.
		std::string Texture;

		// The material's flat colour, multiplied into whatever the texture
		// gives — red, green, blue, alpha.
		//
		// **Carried because dropping it loses the model.** Every format this
		// imports from has one: glTF calls it `baseColorFactor` and PMX calls
		// it the diffuse colour, and an untextured model is *nothing but* these
		// — the fox this was built against has ten material runs, no texture at
		// all, and is recognisably a fox only because each run is a different
		// colour. Without this field it would import as a uniformly tinted
		// blob, which looks exactly like a broken importer.
		//
		// White is the identity, so a mesh that says nothing about colour
		// samples its texture unchanged.
		float BaseColour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	};

	// A mesh, ready to upload.
	//
	// @since v0.9
	struct MeshData {
		// The vertices, referenced by `Indices`.
		std::vector<MeshVertex> Vertices;

		// Triangle list, three indices per triangle, counter-clockwise seen
		// from outside — the winding `render`'s pipeline states as
		// `FRONTFACE_COUNTER_CLOCKWISE` with `CULLMODE_BACK`.
		//
		// **32-bit, and 16 was never an option.** A PMX character is well past
		// 65535 vertices, and a format that could not carry one would have made
		// the importer split a model into pieces to satisfy an index width.
		std::vector<uint32_t> Indices;

		// The material runs. A mesh with none is drawn whole with the
		// consumer's default.
		std::vector<Submesh> Submeshes;

		// The object-space bounding box, derived rather than stored.
		//
		// **Nothing on disk says what these are**, and that is the decision
		// worth keeping: a stored bound is a second copy of a fact the vertices
		// already carry, and the copy is the one an attacker gets to choose. A
		// mesh claiming a bound of zero disappears from every frustum test; one
		// claiming a bound of a kilometre is drawn from everywhere. Both are
		// invisible failures that look like renderer bugs. `Read` computes
		// them, so there is one answer and it is the true one.
		core::Vector3 Minimum;
		core::Vector3 Maximum;

		// Whether this describes a mesh at all.
		//
		// Checks what a consumer would otherwise crash on: a triangle list
		// whose length is not a multiple of three, an index past the end of the
		// vertices, a submesh run reaching past the end of the indices, and a
		// non-finite coordinate. It deliberately does not check that normals
		// are unit length or that the submeshes cover every index — both are
		// authoring quality rather than safety, and refusing them would mean
		// refusing real models over something a consumer handles fine.
		//
		// @return `true` when every index and every run is in range and no
		//         coordinate is a NaN or an infinity.
		bool IsValid() const;

		// Recomputes `Minimum` and `Maximum` from the vertices.
		//
		// An empty mesh gets a zero box rather than the inverted one the fold
		// starts from, because an inverted box propagates into a world AABB and
		// makes a containment test answer nonsense.
		void ComputeBounds();
	};

	// Reading and writing the mesh format.
	//
	// Static, for `Texture`'s reason: a mesh has no state.
	//
	// @since v0.9
	class Mesh {
	  public:
		// "AMS1". A file that is not this format has to fail as "not this
		// format" rather than as a plausible count of nine hundred million.
		static constexpr uint32_t MAGIC = 0x31534D41;

		// The version. Bumped when the layout changes, never reused.
		static constexpr uint16_t VERSION = 1;

		// The most vertices this will read.
		//
		// A bound on what a corrupt count can make a reader allocate, and the
		// same reasoning `Texture::MAXIMUM_DIMENSION` carries. Four million at
		// 32 bytes each is 128 MB, which is far past any single mesh anybody
		// should publish and far below what would take a machine down before
		// the check ran.
		static constexpr uint32_t MAXIMUM_VERTICES = 4u * 1024u * 1024u;

		// The most indices this will read. Sixteen million is four times the
		// vertex ceiling, which is roughly the ratio a well-shared mesh has.
		static constexpr uint32_t MAXIMUM_INDICES = 16u * 1024u * 1024u;

		// The most material runs this will read. A PMX character uses a few
		// dozen; a thousand is a model that should have been several.
		static constexpr uint32_t MAXIMUM_SUBMESHES = 1024;

		// The longest material or texture name this will read, in bytes.
		static constexpr uint32_t MAXIMUM_MATERIAL_BYTES = 128;

		// Writes a mesh.
		//
		// @param writer Where the bytes go.
		// @param data   The mesh. An invalid one writes nothing.
		// @return `false` when `data` is not a valid mesh or is past a ceiling.
		static bool Write(core::ByteWriter &writer, const MeshData &data);

		// Reads a mesh, refusing anything that is not one.
		//
		// Refuses a wrong magic, an unknown version, a count past a ceiling, a
		// count that does not fit in the bytes actually present, an index past
		// the vertices, a submesh run past the indices, an index count that is
		// not a multiple of three, an over-long material name, and any
		// non-finite coordinate. **Every count is checked against what remains
		// before anything is allocated**, which is the difference between a
		// refusal and a decompression bomb.
		//
		// @param reader The bytes to parse.
		// @param out    Filled in on success, left alone otherwise — so a
		//               caller reusing one cannot act on a mixture of the last
		//               good mesh and a bad one.
		// @return `false` on anything malformed. Drop it and count it.
		static bool Read(core::ByteReader &reader, MeshData &out);
	};
}
