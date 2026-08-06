#pragma once

// Baked mesh data. Runtime code uploads this format; import happens in `bake`.
// @tier L8 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::assets {

	// Stable vertex-buffer layout using plain float arrays.
	struct MeshVertex {
		// Object-space position.
		float Position[3];

		// Object-space normal; need not be unit length.
		float Normal[3];

		// Texture coordinate; importers normalize `v` to image row order.
		float TexCoord[2];
	};

	// One run of indices drawn with one material.
	struct Submesh {
		// Where this run starts, as an index into `MeshData::Indices`.
		uint32_t FirstIndex = 0;

		// How many indices it covers. Always a multiple of three.
		uint32_t IndexCount = 0;

		// Source material name. Empty selects the default material.
		std::string Material;

		// Published texture asset name. Empty means no texture.
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
		// White is the identity colour.
		float BaseColour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	};

	// A mesh ready to upload.
	struct MeshData {
		// The vertices, referenced by `Indices`.
		std::vector<MeshVertex> Vertices;

		// Triangle list, counter-clockwise when viewed from outside.
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
