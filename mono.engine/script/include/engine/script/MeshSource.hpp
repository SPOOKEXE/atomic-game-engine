#pragma once

// Turns a decoded mesh asset into the readable copy `ContentService:ReadMesh`
// hands to scripts. Called once per arriving mesh by whatever decoded it, so
// every world that receives the mesh shares one copy.
//
// @tier L9 · shared

#include <engine/assets/Mesh.hpp>
#include <engine/scene/content/MeshCatalogue.hpp>

#include <memory>

namespace engine::script {
	// Packs a mesh's vertices, indices, material runs and skeleton.
	//
	// @param mesh A mesh `assets::Mesh::Read` accepted.
	// @return The shared readable copy.
	// @since v0.26
	std::shared_ptr<const scene::MeshSource> BuildMeshSource(const assets::MeshData &mesh);
}
