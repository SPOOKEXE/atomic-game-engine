#pragma once

// Deterministic triangle reduction for baked meshes.
//
// This operates on `MeshData`, not on a renderer buffer, because a generated
// LOD is a normal mesh asset that must be valid before it reaches a GPU.
// @tier L8 · shared

#include <engine/assets/Mesh.hpp>

#include <span>

namespace engine::assets {
	// How a generated mesh ladder ranks legal reductions.
	//
	// Decimation keeps the historical shortest-edge result. SurfaceArea keeps
	// faces that contribute the most expected projected area, which is the
	// view-independent proxy for a triangle's screen-space importance.
	enum class MeshReduction : uint8_t {
		Decimation,
		SurfaceArea,
	};

	// Produces a coarser mesh while retaining material runs and safe skinning.
	//
	// Collapses only edges whose endpoints have identical skin influences and
	// belong to one submesh. That keeps a joint palette meaningful and prevents
	// one material run from moving another run's vertices. The result has its
	// bounds derived from its surviving vertices.
	//
	// @param source Input mesh. Must be valid.
	// @param ratio Fraction of triangles to retain, in the range (0, 1].
	// @param out Filled on success. May not alias `source`.
	// @return `false` for an invalid input, ratio, alias, or a mesh that cannot
	//         retain at least one triangle in every populated submesh.
	bool DecimateMesh(const MeshData &source, float ratio, MeshData &out);

	// Produces a coarser mesh by removing the least visible surface first.
	//
	// A bake has no camera, so it cannot know one frame's exact projected
	// triangle area. Under uniformly distributed view directions, expected
	// projected area is proportional to object-space surface area. This keeps
	// the large faces that will occupy the most screen space across views while
	// retaining the same winding, material, and skinning guards as DecimateMesh.
	bool ReduceMesh(const MeshData &source, float ratio, MeshData &out);

	// Builds every generated mesh in one LOD ladder from the same base mesh.
	//
	// Each output uses the matching fraction of the source triangle count. This
	// keeps authored levels independent: changing one ratio cannot compound into
	// a different result at the next level.
	//
	// @param source Source mesh. Must be valid.
	// @param ratios Fraction retained for each output, each in (0, 1].
	// @param out One destination per ratio. No destination may alias `source`.
	// @return `false` when the spans differ, an input is invalid, or a level
	//         cannot be decimated.
	bool BuildMeshLodLadder(const MeshData &source, std::span<const float> ratios, std::span<MeshData> out);

	// Builds an area-weighted automatic LOD ladder from one base mesh.
	bool
	BuildReducedMeshLodLadder(const MeshData &source, std::span<const float> ratios, std::span<MeshData> out);
}
