#include <engine/assets/Builtin.hpp>
#include <engine/assets/MeshDecimate.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/AutomaticMeshLod.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace engine::render {
	namespace {
		std::vector<core::Name> ArtifactNames(ecs::Store &store, const core::Name &base) {
			std::vector<core::Name> names;
			store.Each<const scene::Visual, const scene::AutoMeshLOD>(
				[&](ecs::Entity, const scene::Visual &visual, const scene::AutoMeshLOD &automatic) {
					if (visual.Mesh != base || (automatic.Strategy != scene::LodStrategy::Decimated &&
												automatic.Strategy != scene::LodStrategy::Reduced))
						return;
					const uint8_t levels =
						static_cast<uint8_t>(std::clamp<size_t>(automatic.Levels, 1, scene::LOD_LEVELS));
					for (size_t slot = 0; slot < scene::LOD_LEVELS - 1; slot++) {
						const uint8_t level = static_cast<uint8_t>(slot + 1);
						if (level >= levels || automatic.Meshes[slot].IsValid()) continue;
						const core::Name name = scene::AutoMeshLodArtifactName(
							base, level, automatic.Ratios[slot], automatic.Strategy
						);
						if (name.IsValid()) names.push_back(name);
					}
				}
			);
			std::sort(names.begin(), names.end(), [](core::Name left, core::Name right) {
				return left.Id() < right.Id();
			});
			names.erase(std::unique(names.begin(), names.end()), names.end());
			return names;
		}

		template <typename Release>
		size_t RefreshBuilt(
			ecs::Store &store,
			Renderer &renderer,
			const core::Name &base,
			const assets::MeshData &mesh,
			const std::vector<core::Name> &wanted,
			std::vector<core::Name> &artifacts,
			bool &changed,
			core::Name owner,
			Release &&release
		) {
			if (!changed && artifacts == wanted) return 0;
			std::vector<core::Name> released;
			for (const core::Name &name : artifacts) {
				if (!std::binary_search(
						wanted.begin(), wanted.end(), name, [](core::Name left, core::Name right) {
							return left.Id() < right.Id();
						}
					)) {
					released.push_back(name);
				}
			}
			artifacts = wanted;
			release(released);
			const auto generated = BuildAutomaticMeshLods(store, base, mesh);
			size_t uploaded = 0;
			for (const AutomaticMeshLodArtifact &artifact : generated) {
				if (!renderer.AddMesh(artifact.Name, artifact.Data, owner)) continue;
				scene::RecordMesh(
					store, artifact.Name, static_cast<uint32_t>(artifact.Data.Indices.size() / 3)
				);
				uploaded++;
			}
			// A failed derivation or upload must be attempted again. Its requested
			// name alone does not prove that a drawable resource is resident.
			changed = generated.size() != wanted.size() || uploaded != generated.size();
			return uploaded;
		}
	}

	std::vector<AutomaticMeshLodArtifact>
	BuildAutomaticMeshLods(ecs::Store &store, const core::Name &base, const assets::MeshData &mesh) {
		std::vector<AutomaticMeshLodArtifact> generated;
		if (!base.IsValid() || !mesh.IsValid()) return generated;
		store.Each<const scene::Visual, const scene::AutoMeshLOD>([&](ecs::Entity,
																	  const scene::Visual &visual,
																	  const scene::AutoMeshLOD &automatic) {
			if (visual.Mesh != base || (automatic.Strategy != scene::LodStrategy::Decimated &&
										automatic.Strategy != scene::LodStrategy::Reduced))
				return;
			const uint8_t levels =
				static_cast<uint8_t>(std::clamp<size_t>(automatic.Levels, 1, scene::LOD_LEVELS));
			for (size_t slot = 0; slot < scene::LOD_LEVELS - 1; slot++) {
				const uint8_t level = static_cast<uint8_t>(slot + 1);
				if (level >= levels || automatic.Meshes[slot].IsValid()) continue;
				const core::Name name =
					scene::AutoMeshLodArtifactName(base, level, automatic.Ratios[slot], automatic.Strategy);
				if (!name.IsValid() ||
					std::any_of(generated.begin(), generated.end(), [&](const auto &artifact) {
						return artifact.Name == name;
					}))
					continue;
				assets::MeshData output;
				const bool built = automatic.Strategy == scene::LodStrategy::Reduced
									   ? assets::ReduceMesh(mesh, automatic.Ratios[slot], output)
									   : assets::DecimateMesh(mesh, automatic.Ratios[slot], output);
				if (built) generated.push_back({name, std::move(output), {}});
			}
		});
		return generated;
	}

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

	size_t AutomaticMeshLodUploader::RefreshSource(
		ecs::Store &store,
		Renderer &renderer,
		const core::Name &base,
		const assets::MeshData &mesh,
		core::Name owner
	) {
		if (!base.IsValid() || !mesh.IsValid()) return 0;
		auto scope = std::find_if(Scopes.begin(), Scopes.end(), [&](const Scope &entry) {
			return entry.World == store.Identity() && entry.Owner == owner;
		});
		if (scope == Scopes.end()) scope = Scopes.emplace(Scopes.end(), Scope{store.Identity(), owner, {}});
		auto source = std::find_if(scope->Sources.begin(), scope->Sources.end(), [&](const Source &entry) {
			return entry.Base == base;
		});
		if (source == scope->Sources.end())
			source = scope->Sources.emplace(
				scope->Sources.end(), Source{.Base = base, .Artifacts = {}, .Changed = true}
			);
		else
			source->Changed = true;
		scene::RecordMesh(store, base, static_cast<uint32_t>(mesh.Indices.size() / 3));
		const std::vector<core::Name> wanted = ArtifactNames(store, base);
		return RefreshBuilt(
			store,
			renderer,
			base,
			mesh,
			wanted,
			source->Artifacts,
			source->Changed,
			owner,
			[this, &store, &renderer, owner](std::span<const core::Name> released) {
				ReleaseArtifacts(store, renderer, owner, released);
			}
		);
	}

	size_t AutomaticMeshLodUploader::Refresh(
		ecs::Store &store, Renderer &renderer, core::Name owner, bool includeEditable
	) {
		auto scope = std::find_if(Scopes.begin(), Scopes.end(), [&](const Scope &entry) {
			return entry.World == store.Identity() && entry.Owner == owner;
		});
		if (scope == Scopes.end()) scope = Scopes.emplace(Scopes.end(), Scope{store.Identity(), owner, {}});
		std::unordered_map<uint32_t, std::vector<core::Name>> requested;
		std::vector<core::Name> demanded;
		store.Each<const scene::Visual, const scene::AutoMeshLOD>(
			[&](ecs::Entity, const scene::Visual &visual, const scene::AutoMeshLOD &automatic) {
				if (!visual.Mesh.IsValid()) return;
				if (!includeEditable && visual.Mesh.Text().starts_with("editable-mesh://")) return;
				auto [entry, inserted] = requested.try_emplace(visual.Mesh.Id());
				if (inserted) demanded.push_back(visual.Mesh);
				if (automatic.Strategy != scene::LodStrategy::Decimated &&
					automatic.Strategy != scene::LodStrategy::Reduced)
					return;
				const uint8_t levels =
					static_cast<uint8_t>(std::clamp<size_t>(automatic.Levels, 1, scene::LOD_LEVELS));
				for (size_t slot = 0; slot < scene::LOD_LEVELS - 1; slot++) {
					const uint8_t level = static_cast<uint8_t>(slot + 1);
					if (level >= levels || automatic.Meshes[slot].IsValid()) continue;
					const core::Name name = scene::AutoMeshLodArtifactName(
						visual.Mesh, level, automatic.Ratios[slot], automatic.Strategy
					);
					if (name.IsValid()) entry->second.push_back(name);
				}
			}
		);
		for (auto &[_, names] : requested) {
			std::sort(names.begin(), names.end(), [](core::Name left, core::Name right) {
				return left.Id() < right.Id();
			});
			names.erase(std::unique(names.begin(), names.end()), names.end());
		}
		std::sort(demanded.begin(), demanded.end(), [](core::Name left, core::Name right) {
			return left.Id() < right.Id();
		});
		for (const core::Name &base : demanded) {
			if (std::find_if(scope->Sources.begin(), scope->Sources.end(), [&](const Source &source) {
					return source.Base == base;
				}) == scope->Sources.end())
				scope->Sources.push_back({.Base = base, .Artifacts = {}, .Changed = true});
		}
		std::vector<core::Name> released;
		std::erase_if(scope->Sources, [&](const Source &source) {
			if (std::binary_search(
					demanded.begin(), demanded.end(), source.Base, [](core::Name left, core::Name right) {
						return left.Id() < right.Id();
					}
				))
				return false;
			released.insert(released.end(), source.Artifacts.begin(), source.Artifacts.end());
			return true;
		});
		ReleaseArtifacts(store, renderer, owner, released);
		size_t uploaded = 0;
		for (Source &source : scope->Sources)
			uploaded += RefreshStored(store, renderer, source, requested.at(source.Base.Id()), owner);
		return uploaded;
	}

	bool AutomaticMeshLodUploader::ResolveSource(
		ecs::Store &store, Renderer &renderer, const Source &source, core::Name owner, assets::MeshData &out
	) {
		assets::BuiltinMesh builtin;
		if (assets::BuiltinFromName(source.Base.Text(), builtin)) {
			out = assets::MakeBuiltin(builtin);
			return true;
		}
		store.Each<const scene::EditableMesh>([&](ecs::Entity entity, const scene::EditableMesh &editable) {
			if (out.IsValid() || scene::EditableMeshContentName(store, entity) != source.Base) return;
			out = BuildMeshData(editable);
		});
		if (out.IsValid()) return true;
		return renderer.CopyMesh(source.Base, out, 16 * 1024 * 1024, 48 * 1024 * 1024, owner) ==
			   MeshCopyStatus::Copied;
	}

	size_t AutomaticMeshLodUploader::RefreshStored(
		ecs::Store &store,
		Renderer &renderer,
		Source &source,
		const std::vector<core::Name> &wanted,
		core::Name owner
	) {
		if (!source.Changed && source.Artifacts == wanted) return 0;
		if (wanted.empty()) {
			const std::vector<core::Name> released = std::move(source.Artifacts);
			source.Artifacts.clear();
			source.Changed = false;
			ReleaseArtifacts(store, renderer, owner, released);
			return 0;
		}
		assets::MeshData mesh;
		if (!ResolveSource(store, renderer, source, owner, mesh)) return 0;
		scene::RecordMesh(store, source.Base, static_cast<uint32_t>(mesh.Indices.size() / 3));
		return RefreshBuilt(
			store,
			renderer,
			source.Base,
			mesh,
			wanted,
			source.Artifacts,
			source.Changed,
			owner,
			[this, &store, &renderer, owner](std::span<const core::Name> released) {
				ReleaseArtifacts(store, renderer, owner, released);
			}
		);
	}

	bool AutomaticMeshLodUploader::RetainsArtifact(core::Name owner, core::Name artifact) const {
		return std::any_of(Scopes.begin(), Scopes.end(), [&](const Scope &scope) {
			if (scope.Owner != owner) return false;
			return std::any_of(scope.Sources.begin(), scope.Sources.end(), [&](const Source &source) {
				return std::binary_search(
					source.Artifacts.begin(),
					source.Artifacts.end(),
					artifact,
					[](core::Name left, core::Name right) { return left.Id() < right.Id(); }
				);
			});
		});
	}

	void AutomaticMeshLodUploader::ReleaseArtifacts(
		ecs::Store &store, Renderer &renderer, core::Name owner, std::span<const core::Name> artifacts
	) {
		for (const core::Name &artifact : artifacts) {
			if (!RetainsArtifact(owner, artifact)) (void)renderer.DropMesh(artifact, owner);
			(void)scene::ForgetMesh(store, artifact);
		}
	}

	void AutomaticMeshLodUploader::ForgetWorld(uint64_t identity) {
		std::erase_if(Scopes, [identity](const Scope &scope) { return scope.World == identity; });
	}

	void AutomaticMeshLodUploader::ForgetOwner(core::Name owner) {
		std::erase_if(Scopes, [owner](const Scope &scope) { return scope.Owner == owner; });
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
