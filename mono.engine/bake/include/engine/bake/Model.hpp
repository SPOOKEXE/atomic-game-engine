#pragma once

// Reading somebody else's model format, so the engine never has to.
//
// `assets::Mesh` is what this produces and `Image.hpp` is its twin for pixels.
// The same sentence governs both: **a runtime does not import.** A glTF reader
// is a JSON parser plus an accessor walker plus a node-hierarchy fold, and none
// of that belongs on the frame a mesh streams in.
//
// **What an importer produces is one mesh, not a scene.** A `.glb` may hold a
// hierarchy of nodes, several meshes and a skeleton; what comes out here is a
// single `assets::MeshData` with the node transforms already baked into the
// vertices and one submesh per material. That is a deliberate narrowing rather
// than a simplification of convenience: `scene` is where a hierarchy lives in
// this engine, and an importer that produced instances would be authoring a
// world from a file format's opinion about one.
//
// **Skinning is dropped and the rest pose is kept.** There are no skeletons in
// the engine yet, so a skinned mesh imports as the pose its vertices are
// actually stored in. `ROADMAP.md` has animation later; the day it arrives,
// this is where joints and weights start being carried rather than skipped.
//
// @tier L9 · shared

#include <engine/assets/Mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bake {

	// Which model format some bytes are.
	//
	// @since v0.9
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

	// An image that travelled inside the model rather than beside it.
	//
	// **GLB embeds its textures far more often than it references them**, and an
	// importer that only handled the referenced case would import the common
	// file as untextured — which looks like a texture-binding bug three layers
	// away. So the bytes come out here and the caller bakes them like any other
	// image.
	//
	// @since v0.9
	struct EmbeddedImage {
		// What the model called it, or a derived name when it called it
		// nothing. Unique within one import.
		std::string Name;

		// The image file, still in whatever format it was embedded as — PNG or
		// JPEG in practice. Undecoded on purpose: `ReadImage` is the one place
		// that decides what an image is.
		std::vector<std::byte> Bytes;
	};

	// What an importer produces.
	//
	// @since v0.9
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
	};

	// Identifies a format from the leading bytes.
	//
	// **OBJ is never the answer**, because it has no signature — it is a text
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
	// whatever its author's grid was — so a scene that placed them together
	// would have one filling the sky. Fitting is uniform, so nothing is
	// stretched, and the recentre is what makes a part's `CFrame` mean the
	// middle of the thing rather than wherever its author left the origin.
	//
	// A degenerate mesh — every vertex at one point — is left alone rather than
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
	// corner actually belongs to — which shows up as a dark crease along an
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
