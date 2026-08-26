#pragma once

// Model import to one `assets::MeshData`; node transforms are baked in.
// Skinning remains in the rest pose.
// @tier L9 · shared

#include <engine/assets/Mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bake {

	// Model format.
	enum class ModelFormat : uint8_t {
		// Not a format this reads.
		Unknown,

		// glTF 2.0, as a binary `.glb` container or as raw `.gltf` JSON.
		Gltf,

		// Wavefront OBJ. Text, and the one format here with no header at all.
		Obj,

		// PMX 2.0 and 2.1, the MikuMikuDance model format.
		Pmx,
	};

	// An image embedded in the model.
	struct EmbeddedImage {
		// What the model called it, or a derived name when it called it
		// nothing. Unique within one import.
		std::string Name;

		// The image file, still in whatever format it was embedded as - PNG or
		// JPEG in practice. Undecoded on purpose: `ReadImage` is the one place
		// that decides what an image is.
		std::vector<std::byte> Bytes;
	};

	// Import result.
	struct ImportedModel {
		// The geometry, with node transforms baked in and bounds computed.
		assets::MeshData Mesh;

		// Images that arrived inside the model, in first-use order.
		std::vector<EmbeddedImage> Embedded;

		// Files the materials referenced, **as the model spells them** and
		// relative to wherever the model came from.
		//
		// Not resolved to asset names here, and that is the seam: this module
		// has no filesystem and no opinion about where a publisher puts things.
		// `Submesh::Texture` holds the same spelling, so the caller rewrites
		// both together once it knows what it published.
		std::vector<std::string> Textures;

		// The material library an OBJ named, **as the model spells it**, or
		// empty for a format that carries its materials inside itself.
		//
		// **A name and not the contents, for the reason `Textures` above is a
		// name.** An `.obj` puts its materials in a second file and says
		// `mtllib box.mtl`; opening that file is a filesystem operation and this
		// module deliberately has none. So the importer reports what the model
		// asked for and the publisher - which does know where the model came
		// from - reads it and fills in `Submesh::Texture` and
		// `Submesh::BaseColour`.
		//
		// Empty for glTF and PMX: both carry their materials in the same file
		// and set those fields themselves.
		std::string MaterialLibrary;
	};

	// Identifies a format from the leading bytes.
	//
	// **OBJ is never the answer**, because it has no signature - it is a text
	// file whose first line may be a comment. `ModelFormatOfName` is what
	// recognises one, and that asymmetry is stated rather than papered over
	// with a heuristic that would misfire on a stray text file.
	//
	// @param bytes The file, or as much of its front as is available.
	// @return The format, or `Unknown`.
	ModelFormat ModelFormatOfBytes(std::span<const std::byte> bytes);

	// Identifies a format from a file name's extension.
	//
	// @param name The file name.
	// @return The format, or `Unknown`.
	ModelFormat ModelFormatOfName(std::string_view name);

	// A name for a format, for a log line and a command line.
	//
	// @param format The format.
	// @return A view valid for the lifetime of the process.
	std::string_view Describe(ModelFormat format);

	// Imports a model.
	//
	// The format is passed rather than sniffed so that OBJ is reachable at all;
	// a caller that has a name should ask `ModelFormatOfName` first and fall
	// back to `ModelFormatOfBytes`.
	//
	// Every count in every one of these formats is treated as hostile, for
	// `Image.hpp`'s reason: this runs over a directory somebody uploaded.
	//
	// @param format  Which reader to use.
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, so a bake tool can name the file
	//                *and* the reason. Untouched on success.
	// @return `false` on anything this cannot read or will not trust.
	bool
	ReadModel(ModelFormat format, std::span<const std::byte> bytes, ImportedModel &out, std::string &failure);

	// Scales and recentres a mesh so its longest axis measures `size` and its
	// box is centred on the origin.
	//
	// **The step that makes an imported model usable at all.** A PMX character
	// is about twenty units tall, a glTF one is about two, and an OBJ is
	// whatever its author's grid was - so a scene that placed them together
	// would have one filling the sky. Fitting is uniform, so nothing is
	// stretched, and the recentre is what makes a part's `CFrame` mean the
	// middle of the thing rather than wherever its author left the origin.
	//
	// A degenerate mesh - every vertex at one point - is left alone rather than
	// scaled by infinity.
	//
	// @param mesh The mesh, changed in place. Bounds are recomputed.
	// @param size The target measurement of the longest axis, in metres.
	// @return `false` for a non-positive size or a mesh with no extent.
	bool FitMesh(assets::MeshData &mesh, float size);

	// Replaces every normal with the area-weighted average of the faces meeting
	// at that vertex.
	//
	// **Area-weighted rather than face-count-weighted**, because a fan of
	// slivers at one corner would otherwise outvote the two large faces the
	// corner actually belongs to - which shows up as a dark crease along an
	// edge that should be flat.
	//
	// Only useful for a model that arrived without normals, or one whose
	// normals a scale has invalidated. A mesh whose vertices are already split
	// per face is unchanged by this, which is why it is safe to run over
	// anything.
	//
	// @param mesh The mesh, changed in place.
	void SmoothNormals(assets::MeshData &mesh);
}
