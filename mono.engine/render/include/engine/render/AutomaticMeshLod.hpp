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

namespace engine::ecs {
	class Store;
}

namespace engine::render {
	class Renderer;

	// One derived mesh level and the worlds that requested it.
	struct AutomaticMeshLodArtifact {
		// Content name shared by matching source meshes.
		core::Name Name;
		// Host-side mesh bytes awaiting renderer upload.
		assets::MeshData Data;
		// Worlds that should retain this derived level.
		std::vector<world::WorldId> Worlds;
	};

	// Builds every missing automatic level requested by the selected worlds.
	//
	// Matching inputs share one artifact. This function only reads world state
	// and builds host data, so callers can test planning without a GPU.
	std::vector<AutomaticMeshLodArtifact>
	BuildAutomaticMeshLods(ecs::Store &store, const core::Name &base, const assets::MeshData &mesh);

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

	class AutomaticMeshLodUploader {
	  public:
		size_t
		Refresh(ecs::Store &store, Renderer &renderer, core::Name owner = {}, bool includeEditable = true);
		size_t RefreshSource(
			ecs::Store &store,
			Renderer &renderer,
			const core::Name &base,
			const assets::MeshData &mesh,
			core::Name owner = {}
		);
		void ForgetWorld(uint64_t identity);
		void ForgetOwner(core::Name owner);

	  private:
		struct Source {
			core::Name Base;
			std::vector<core::Name> Artifacts;
			bool Changed = true;
		};
		struct Scope {
			uint64_t World = 0;
			core::Name Owner;
			std::vector<Source> Sources;
		};
		size_t RefreshStored(
			ecs::Store &store,
			Renderer &renderer,
			Source &source,
			const std::vector<core::Name> &wanted,
			core::Name owner
		);
		bool ResolveSource(
			ecs::Store &store,
			Renderer &renderer,
			const Source &source,
			core::Name owner,
			assets::MeshData &out
		);
		bool RetainsArtifact(core::Name owner, core::Name artifact) const;
		void ReleaseArtifacts(
			ecs::Store &store, Renderer &renderer, core::Name owner, std::span<const core::Name> artifacts
		);
		std::vector<Scope> Scopes;
	};
}
