#include <engine/assets/MeshDecimate.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/AutomaticMeshLod.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>

#include <algorithm>
#include <array>

namespace engine::render {
	std::vector<AutomaticMeshLodArtifact> BuildAutomaticMeshLods(
		world::Universe &universe,
		std::span<const world::WorldId> worlds,
		const core::Name &base,
		const assets::MeshData &mesh
	) {
		std::vector<AutomaticMeshLodArtifact> generated;
		if (!base.IsValid() || !mesh.IsValid()) return generated;

		for (const world::WorldId id : worlds) {
			universe.Enter(id, [&](ecs::Store &store) {
				store.Each<const scene::Visual, const scene::AutoMeshLOD>(
					[&](ecs::Entity, const scene::Visual &visual, const scene::AutoMeshLOD &automatic) {
						if (visual.Mesh != base || (automatic.Strategy != scene::LodStrategy::Decimated &&
													automatic.Strategy != scene::LodStrategy::Reduced))
							return;

						const uint8_t levels =
							static_cast<uint8_t>(std::clamp<size_t>(automatic.Levels, 1, scene::LOD_LEVELS));
						std::array<float, scene::LOD_LEVELS - 1> ratios;
						std::array<core::Name, scene::LOD_LEVELS - 1> names;
						size_t count = 0;
						for (size_t slot = 0; slot < scene::LOD_LEVELS - 1; slot++) {
							const uint8_t level = static_cast<uint8_t>(slot + 1);
							if (level >= levels || automatic.Meshes[slot].IsValid()) continue;
							const core::Name name = scene::AutoMeshLodArtifactName(
								base, level, automatic.Ratios[slot], automatic.Strategy
							);
							if (!name.IsValid()) continue;
							ratios[count] = automatic.Ratios[slot];
							names[count++] = name;
						}
						if (count == 0) return;

						std::array<assets::MeshData, scene::LOD_LEVELS - 1> ladder;
						const std::span<const float> wanted(ratios.data(), count);
						const std::span<assets::MeshData> outputs(ladder.data(), count);
						const bool built = automatic.Strategy == scene::LodStrategy::Reduced
											   ? assets::BuildReducedMeshLodLadder(mesh, wanted, outputs)
											   : assets::BuildMeshLodLadder(mesh, wanted, outputs);
						if (!built) {
							ENGINE_WARN("render: {} cannot build its automatic mesh LOD ladder", base.Text());
							return;
						}

						for (size_t slot = 0; slot < count; slot++) {
							auto found =
								std::find_if(generated.begin(), generated.end(), [&](const auto &row) {
									return row.Name == names[slot];
								});
							if (found == generated.end()) {
								generated.push_back({names[slot], std::move(ladder[slot]), {id}});
							} else if (std::find(found->Worlds.begin(), found->Worlds.end(), id) ==
									   found->Worlds.end()) {
								found->Worlds.push_back(id);
							}
						}
					}
				);
			});
		}
		return generated;
	}

	size_t PublishAutomaticMeshLods(
		world::Universe &universe,
		Renderer &renderer,
		std::span<const core::Name> owners,
		std::span<const AutomaticMeshLodArtifact> artifacts
	) {
		size_t published = 0;
		for (const AutomaticMeshLodArtifact &artifact : artifacts) {
			bool uploaded = false;
			for (const core::Name owner : owners) {
				uploaded = renderer.AddMesh(artifact.Name, artifact.Data, owner) || uploaded;
			}
			if (!uploaded) continue;
			published++;
			const uint32_t triangles = static_cast<uint32_t>(artifact.Data.Indices.size() / 3);
			for (const world::WorldId id : artifact.Worlds) {
				universe.Enter(id, [&](ecs::Store &store) {
					scene::RecordMesh(store, artifact.Name, triangles);
				});
			}
		}
		return published;
	}
}
