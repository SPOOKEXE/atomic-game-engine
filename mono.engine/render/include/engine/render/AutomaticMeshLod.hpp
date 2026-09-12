#pragma once

// Automatic mesh LOD artifacts built from authored scene policy and published
// through renderer residency.
//
// @tier L12 · client

#include <engine/assets/Mesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/world/Universe.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace engine::render {
	class Renderer;

	struct AutomaticMeshLodArtifact {
		core::Name Name;
		assets::MeshData Data;
		std::vector<world::WorldId> Worlds;
	};

	// Builds every missing automatic level requested by the selected worlds.
	//
	// Matching inputs share one artifact. This function only reads world state
	// and builds host data, so callers can test planning without a GPU.
	std::vector<AutomaticMeshLodArtifact> BuildAutomaticMeshLods(
		world::Universe &universe,
		std::span<const world::WorldId> worlds,
		const core::Name &base,
		const assets::MeshData &mesh
	);

	// Uploads a built batch and records each admitted artifact in its worlds.
	//
	// Returns the number of artifacts admitted by at least one content owner.
	size_t PublishAutomaticMeshLods(
		world::Universe &universe,
		Renderer &renderer,
		std::span<const core::Name> owners,
		std::span<const AutomaticMeshLodArtifact> artifacts
	);
}
