#pragma once
#include <engine/imagegraph/Document.hpp>

#include <span>

namespace engine::imagegraph::detail {
	inline std::span<const MeshPart3D> SourceCpuMeshParts(const MeshData3D &mesh) {
		return mesh.CpuVerticesPresent ? std::span<const MeshPart3D>{mesh.Parts}
									   : std::span<const MeshPart3D>{};
	}
	inline std::span<const MeshEdge3D> SourceCpuMeshEdges(const MeshData3D &mesh) {
		return mesh.CpuEdgesPresent ? std::span<const MeshEdge3D>{mesh.Edges} : std::span<const MeshEdge3D>{};
	}
	// Source object clones retain drawable buffers, clear CPU edges and omit VBM.
	inline void ApplySourceObjectCloneMetadata(MeshData3D &mesh, bool vertices) {
		mesh.CpuVerticesPresent = vertices && mesh.CpuVerticesPresent;
		mesh.CpuEdgesPresent = false;
		for (auto &part : mesh.Parts)
			part.LocalMatrix.reset();
	}
	// The caller admits the retained copy before allocating this owned value.
	inline MeshValue3D CloneSourceObjectMesh(const MeshValue3D &source, bool vertices) {
		MeshValue3D result = source;
		if (!result.Data) return result;
		ApplySourceObjectCloneMetadata(*result.Data, vertices);
		return result;
	}
	// Source build returns before rebuilding edge buffers when CPU vertices are empty.
	inline void RebuildSourceObjectMesh(MeshData3D &mesh) {
		if (!mesh.CpuVerticesPresent || mesh.Parts.empty()) {
			mesh.Parts.clear();
			return;
		}
		if (!mesh.CpuEdgesPresent) mesh.Edges.clear();
	}
}
