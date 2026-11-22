#include <engine/assets/Builtin.hpp>
#include <engine/assets/MeshDecimate.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/resources/AutomaticMeshLod.hpp>
#include <engine/render/resources/EditableMeshes.hpp>
#include <engine/render/resources/MeshTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/content/MeshCatalogue.hpp>
#include <engine/scene/editable/EditableMesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace engine::render {
	AutomaticMeshLodUploader::Job::~Job() {
		RequestCancel();
		if (Worker.joinable()) Worker.join();
	}

	std::vector<AutomaticMeshLodArtifact>
	BuildAutomaticMeshLods(ecs::Store &store, const core::Name &base, const assets::MeshData &mesh) {
		std::vector<AutomaticMeshLodArtifact> generated;
		if (!base.IsValid() || !mesh.IsValid()) return generated;
		store.Each<const scene::Visual, const scene::LODAuto>([&](ecs::Entity,
																  const scene::Visual &visual,
																  const scene::LODAuto &automatic) {
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
				if (built) generated.push_back({name, std::move(output), {}, {}});
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
				store.Each<const scene::Visual, const scene::LODAuto>([&](ecs::Entity,
																		  const scene::Visual &visual,
																		  const scene::LODAuto &automatic) {
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
						auto found = std::find_if(generated.begin(), generated.end(), [&](const auto &row) {
							return row.Name == names[slot];
						});
						if (found == generated.end()) {
							generated.push_back({names[slot], std::move(ladder[slot]), {id}, {}});
						} else if (std::find(found->Worlds.begin(), found->Worlds.end(), id) ==
								   found->Worlds.end()) {
							found->Worlds.push_back(id);
						}
					}
				});
			});
		}
		return generated;
	}

	size_t AutomaticMeshLodUploader::RefreshSource(
		ecs::Store &store,
		Renderer &renderer,
		const core::Name &base,
		const assets::MeshData &mesh,
		core::Name owner,
		std::span<const uint32_t> welds
	) {
		if (!base.IsValid() || !mesh.IsValid()) return 0;
		const size_t uploaded = Collect(store, renderer);
		auto scope = std::find_if(Scopes.begin(), Scopes.end(), [&](const Scope &entry) {
			return entry.World == store.Identity() && entry.Owner == owner;
		});
		if (scope == Scopes.end()) scope = Scopes.emplace(Scopes.end(), Scope{store.Identity(), owner, {}});
		auto source = std::find_if(scope->Sources.begin(), scope->Sources.end(), [&](const Source &entry) {
			return entry.Base == base;
		});
		if (source == scope->Sources.end()) source = scope->Sources.emplace(scope->Sources.end());
		if (source->Base != base)
			source->Base = base;
		else
			source->Changed = true;
		scene::RecordMesh(store, base, static_cast<uint32_t>(mesh.Indices.size() / 3));
		const std::vector<Request> wanted = Requests(store, base);
		return uploaded + RefreshStored(store, renderer, *source, wanted, owner, &mesh, welds);
	}

	namespace {
		// One level's streams re-derived from an editable source, or false
		// when the blend names a vertex the source no longer has.
		bool BlendLevel(
			const assets::MeshVertexSources &sources,
			const scene::EditableMesh &mesh,
			std::vector<float> &positions,
			std::vector<float> &normals,
			std::vector<float> &texCoords
		) {
			const size_t count = sources.Count();
			positions.assign(count * 3, 0.0f);
			normals.assign(count * 3, 0.0f);
			texCoords.assign(count * 2, 0.0f);
			for (size_t vertex = 0; vertex < count; vertex++) {
				for (uint32_t at = sources.Offsets[vertex]; at < sources.Offsets[vertex + 1]; at++) {
					const uint32_t from = sources.Vertices[at];
					if (from >= mesh.Positions.size()) return false;
					const float weight = sources.Weights[at];
					positions[vertex * 3] += mesh.Positions[from].X * weight;
					positions[vertex * 3 + 1] += mesh.Positions[from].Y * weight;
					positions[vertex * 3 + 2] += mesh.Positions[from].Z * weight;
					normals[vertex * 3] += mesh.Normals[from].X * weight;
					normals[vertex * 3 + 1] += mesh.Normals[from].Y * weight;
					normals[vertex * 3 + 2] += mesh.Normals[from].Z * weight;
					texCoords[vertex * 2] += mesh.UVs[from].X * weight;
					texCoords[vertex * 2 + 1] += mesh.UVs[from].Y * weight;
				}
				float *normal = normals.data() + vertex * 3;
				const float length =
					std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
				if (length > 0.0f)
					for (size_t axis = 0; axis < 3; axis++)
						normal[axis] /= length;
			}
			return true;
		}

		// The editable mesh a content name belongs to, or null.
		const scene::EditableMesh *FindEditable(ecs::Store &store, core::Name base) {
			const scene::EditableMesh *found = nullptr;
			store.Each<const scene::EditableMesh>([&](ecs::Entity entity, const scene::EditableMesh &mesh) {
				if (found == nullptr && scene::EditableMeshContentName(store, entity) == base) found = &mesh;
			});
			return found;
		}
	}

	bool AutomaticMeshLodUploader::FollowSource(
		ecs::Store &store,
		Renderer &renderer,
		const core::Name &base,
		const scene::EditableMesh &mesh,
		core::Name owner,
		size_t &patched
	) {
		const auto scope = std::find_if(Scopes.begin(), Scopes.end(), [&](const Scope &entry) {
			return entry.World == store.Identity() && entry.Owner == owner;
		});
		if (scope == Scopes.end()) return true;
		const auto source =
			std::find_if(scope->Sources.begin(), scope->Sources.end(), [&](const Source &entry) {
				return entry.Base == base;
			});
		// No level was ever asked for, or one is being built and reads the
		// current streams when it completes.
		if (source == scope->Sources.end() || source->Next ||
			(Active && Active->World == scope->World && Active->Owner == owner && Active->Base == base))
			return true;
		if (!source->UsesProvidedMesh || source->Levels.size() != source->Artifacts.size()) return false;
		std::vector<float> positions;
		std::vector<float> normals;
		std::vector<float> texCoords;
		for (const Followed &level : source->Levels) {
			if (!BlendLevel(level.Sources, mesh, positions, normals, texCoords)) return false;
			MeshVertexPatch patch;
			patch.Count = static_cast<uint32_t>(level.Sources.Count());
			patch.Positions = positions;
			patch.Normals = normals;
			patch.TexCoords = texCoords;
			if (!renderer.PatchMesh(level.Name, patch, owner)) return false;
			patched++;
		}
		return true;
	}

	std::vector<AutomaticMeshLodUploader::Request>
	AutomaticMeshLodUploader::Requests(ecs::Store &store, core::Name base) {
		std::vector<Request> requests;
		store.Each<const scene::Visual, const scene::LODAuto>([&](ecs::Entity,
																  const scene::Visual &visual,
																  const scene::LODAuto &automatic) {
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
				if (name.IsValid()) requests.push_back({name, automatic.Ratios[slot], automatic.Strategy});
			}
		});
		std::sort(requests.begin(), requests.end(), [](const Request &left, const Request &right) {
			return left.Name.Id() < right.Name.Id();
		});
		requests.erase(
			std::unique(
				requests.begin(),
				requests.end(),
				[](const Request &left, const Request &right) { return left.Name == right.Name; }
			),
			requests.end()
		);
		return requests;
	}

	std::vector<AutomaticMeshLodArtifact> AutomaticMeshLodUploader::Build(
		const assets::MeshData &mesh,
		std::span<const Request> requests,
		assets::MeshDecimationCancelToken cancel,
		std::span<const uint32_t> welds,
		bool blend
	) {
		std::vector<AutomaticMeshLodArtifact> artifacts;
		for (const Request &request : requests) {
			if (cancel.StopRequested()) return {};
			assets::MeshData output;
			assets::MeshVertexSources sources;
			const assets::MeshReductionOptions options{welds, blend ? &sources : nullptr};
			const bool built = assets::ReduceMesh(
				mesh,
				request.Ratio,
				output,
				request.Strategy == scene::LodStrategy::Reduced ? assets::MeshReduction::SurfaceArea
																: assets::MeshReduction::Decimation,
				cancel,
				options
			);
			if (cancel.StopRequested()) return {};
			if (built) artifacts.push_back({request.Name, std::move(output), {}, std::move(sources)});
		}
		return artifacts;
	}

	void AutomaticMeshLodUploader::Start(uint64_t world, core::Name owner, Source &source) {
		if (Active || !source.Next) return;
		auto queued = std::move(source.Next);
		Active = std::make_unique<Job>();
		Active->World = world;
		Active->Owner = owner;
		Active->Base = source.Base;
		Active->Generation = queued->Generation;
		Active->SourceRevision = queued->SourceRevision;
		Job *job = Active.get();
		job->Worker = std::thread([job,
								   mesh = std::move(queued->Mesh),
								   requests = std::move(queued->Requests),
								   welds = std::move(queued->Welds),
								   blend = queued->Blend]() mutable {
			const assets::MeshDecimationCancelToken cancel(job->CancelRequested);
			std::vector<AutomaticMeshLodArtifact> result = Build(mesh, requests, cancel, welds, blend);
			std::lock_guard lock(job->Guard);
			job->Result = std::move(result);
			job->Ready = true;
		});
	}

	size_t AutomaticMeshLodUploader::Collect(ecs::Store &store, Renderer &renderer) {
		if (Active) {
			std::lock_guard lock(Active->Guard);
			if (Active->Ready) Completed.push_back(std::move(Active));
		}
		if (!Active) {
			Scope *nextScope = nullptr;
			Source *nextSource = nullptr;
			for (Scope &candidate : Scopes) {
				for (Source &candidateSource : candidate.Sources) {
					if (!candidateSource.Next ||
						(nextSource != nullptr && candidateSource.Next->Ticket >= nextSource->Next->Ticket))
						continue;
					nextScope = &candidate;
					nextSource = &candidateSource;
				}
			}
			if (nextSource != nullptr) Start(nextScope->World, nextScope->Owner, *nextSource);
		}
		auto completed = std::find_if(Completed.begin(), Completed.end(), [&](const auto &job) {
			return job->World == store.Identity();
		});
		if (completed == Completed.end()) return 0;
		std::unique_ptr<Job> job = std::move(*completed);
		Completed.erase(completed);
		std::vector<AutomaticMeshLodArtifact> result;
		{
			std::lock_guard lock(job->Guard);
			result = std::move(job->Result);
		}
		const uint64_t world = job->World;
		const core::Name owner = job->Owner;
		const core::Name base = job->Base;
		const uint64_t generation = job->Generation;
		const uint64_t sourceRevision = job->SourceRevision;
		auto scope = std::find_if(Scopes.begin(), Scopes.end(), [&](const Scope &entry) {
			return entry.World == world && entry.Owner == owner;
		});
		if (scope == Scopes.end()) return 0;
		auto source = std::find_if(scope->Sources.begin(), scope->Sources.end(), [&](const Source &entry) {
			return entry.Base == base;
		});
		if (source == scope->Sources.end() || source->Generation != generation) return 0;
		if (Requests(store, source->Base) != source->Requested) {
			source->Changed = true;
			return 0;
		}
		if (!source->UsesProvidedMesh) {
			uint64_t currentRevision = 0;
			if (!SourceRevision(store, renderer, *source, owner, currentRevision) ||
				currentRevision != sourceRevision) {
				source->Changed = true;
				return 0;
			}
		}

		std::vector<core::Name> wanted;
		wanted.reserve(source->Requested.size());
		for (const Request &request : source->Requested)
			wanted.push_back(request.Name);
		std::vector<core::Name> released;
		for (const core::Name name : source->Artifacts) {
			if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) released.push_back(name);
		}
		source->Artifacts.clear();
		source->Levels.clear();
		ReleaseArtifacts(store, renderer, owner, released);
		// Stream edits since the build was queued only moved vertices, so the
		// levels take the current streams through their blends before upload.
		const scene::EditableMesh *editable =
			source->UsesProvidedMesh ? FindEditable(store, source->Base) : nullptr;
		std::vector<float> positions;
		std::vector<float> normals;
		std::vector<float> texCoords;
		size_t uploaded = 0;
		bool following = true;
		for (AutomaticMeshLodArtifact &artifact : result) {
			const bool blended =
				artifact.Sources.Count() == artifact.Data.Vertices.size() && !artifact.Data.Vertices.empty();
			if (editable != nullptr && blended && !editable->Packing.Enabled() &&
				BlendLevel(artifact.Sources, *editable, positions, normals, texCoords)) {
				for (size_t vertex = 0; vertex < artifact.Data.Vertices.size(); vertex++) {
					std::copy_n(positions.data() + vertex * 3, 3, artifact.Data.Vertices[vertex].Position);
					std::copy_n(normals.data() + vertex * 3, 3, artifact.Data.Vertices[vertex].Normal);
					std::copy_n(texCoords.data() + vertex * 2, 2, artifact.Data.Vertices[vertex].TexCoord);
				}
				artifact.Data.ComputeBounds();
			}
			if (!renderer.AddMesh(artifact.Name, artifact.Data, owner)) continue;
			scene::RecordMesh(store, artifact.Name, static_cast<uint32_t>(artifact.Data.Indices.size() / 3));
			source->Artifacts.push_back(artifact.Name);
			following = following && blended;
			if (following) source->Levels.push_back({artifact.Name, std::move(artifact.Sources)});
			uploaded++;
		}
		if (!following) source->Levels.clear();
		source->Changed = uploaded != source->Requested.size();
		source->SourceRevision = sourceRevision;
		return uploaded;
	}

	size_t AutomaticMeshLodUploader::Refresh(
		ecs::Store &store, Renderer &renderer, core::Name owner, bool includeEditable
	) {
		size_t uploaded = Collect(store, renderer);
		auto scope = std::find_if(Scopes.begin(), Scopes.end(), [&](const Scope &entry) {
			return entry.World == store.Identity() && entry.Owner == owner;
		});
		if (scope == Scopes.end()) scope = Scopes.emplace(Scopes.end(), Scope{store.Identity(), owner, {}});
		std::unordered_map<uint32_t, std::vector<Request>> requested;
		std::vector<core::Name> demanded;
		store.Each<const scene::Visual, const scene::LODAuto>(
			[&](ecs::Entity, const scene::Visual &visual, const scene::LODAuto &automatic) {
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
					if (name.IsValid())
						entry->second.push_back({name, automatic.Ratios[slot], automatic.Strategy});
				}
			}
		);
		for (auto &[_, names] : requested) {
			std::sort(names.begin(), names.end(), [](const Request &left, const Request &right) {
				return left.Name.Id() < right.Name.Id();
			});
			names.erase(
				std::unique(
					names.begin(),
					names.end(),
					[](const Request &left, const Request &right) { return left.Name == right.Name; }
				),
				names.end()
			);
		}
		std::sort(demanded.begin(), demanded.end(), [](core::Name left, core::Name right) {
			return left.Id() < right.Id();
		});
		for (const core::Name &base : demanded) {
			if (std::find_if(scope->Sources.begin(), scope->Sources.end(), [&](const Source &source) {
					return source.Base == base;
				}) == scope->Sources.end()) {
				Source source;
				source.Base = base;
				scope->Sources.push_back(std::move(source));
			}
		}
		std::vector<core::Name> released;
		std::erase_if(scope->Sources, [&](const Source &source) {
			if (std::binary_search(
					demanded.begin(), demanded.end(), source.Base, [](core::Name left, core::Name right) {
						return left.Id() < right.Id();
					}
				))
				return false;
			if (Active && Active->World == scope->World && Active->Owner == owner &&
				Active->Base == source.Base)
				Active->RequestCancel();
			released.insert(released.end(), source.Artifacts.begin(), source.Artifacts.end());
			return true;
		});
		ReleaseArtifacts(store, renderer, owner, released);
		for (Source &source : scope->Sources)
			uploaded += RefreshStored(store, renderer, source, requested.at(source.Base.Id()), owner);
		return uploaded;
	}

	MeshCopyStatus AutomaticMeshLodUploader::ResolveSource(
		ecs::Store &store, Renderer &renderer, const Source &source, core::Name owner, assets::MeshData &out
	) {
		assets::BuiltinMesh builtin;
		if (assets::BuiltinFromName(source.Base.Text(), builtin)) {
			out = assets::MakeBuiltin(builtin);
			return MeshCopyStatus::Copied;
		}
		if (source.Base.Text().starts_with("editable-mesh://")) {
			bool editableFound = false;
			store.Each<const scene::EditableMesh>([&](ecs::Entity entity,
													  const scene::EditableMesh &editable) {
				if (out.IsValid() || scene::EditableMeshContentName(store, entity) != source.Base) return;
				editableFound = true;
				out = BuildMeshData(editable);
			});
			if (out.IsValid()) return MeshCopyStatus::Copied;
			return editableFound ? MeshCopyStatus::Invalid : MeshCopyStatus::Missing;
		}
		return renderer.CopyMesh(source.Base, out, 16 * 1024 * 1024, 48 * 1024 * 1024, owner);
	}

	bool AutomaticMeshLodUploader::SourceRevision(
		ecs::Store &store, Renderer &renderer, const Source &source, core::Name owner, uint64_t &out
	) {
		assets::BuiltinMesh builtin;
		if (assets::BuiltinFromName(source.Base.Text(), builtin)) {
			out = 1;
			return true;
		}
		if (source.Base.Text().starts_with("editable-mesh://")) {
			bool editableFound = false;
			store.Each<const scene::EditableMesh>([&](ecs::Entity entity,
													  const scene::EditableMesh &editable) {
				if (scene::EditableMeshContentName(store, entity) != source.Base) return;
				out = (static_cast<uint64_t>(editable.Revision) << 1u) | 1u;
				editableFound = true;
			});
			return editableFound;
		}
		out = renderer.MeshRevision(source.Base, owner);
		return out != 0;
	}

	void AutomaticMeshLodUploader::InvalidateSource(
		ecs::Store &store, Renderer &renderer, Source &source, core::Name owner
	) {
		++source.Generation;
		if (Active && Active->World == store.Identity() && Active->Owner == owner &&
			Active->Base == source.Base) {
			Active->RequestCancel();
		}
		source.Next.reset();
		const std::vector<core::Name> released = std::move(source.Artifacts);
		source.Artifacts.clear();
		source.Levels.clear();
		ReleaseArtifacts(store, renderer, owner, released);
	}

	size_t AutomaticMeshLodUploader::RefreshStored(
		ecs::Store &store,
		Renderer &renderer,
		Source &source,
		const std::vector<Request> &wanted,
		core::Name owner,
		const assets::MeshData *provided,
		std::span<const uint32_t> welds
	) {
		if (wanted.empty()) {
			InvalidateSource(store, renderer, source, owner);
			source.Requested.clear();
			source.Changed = false;
			return 0;
		}
		if (!source.Changed && source.Requested == wanted) {
			if (source.UsesProvidedMesh) return 0;
			uint64_t revision = 0;
			if (SourceRevision(store, renderer, source, owner, revision) && source.SourceRevision == revision)
				return 0;
			if (revision == 0) {
				InvalidateSource(store, renderer, source, owner);
				source.Requested.clear();
				return 0;
			}
			// A base replacement changes only this source's revision. Its old
			// ladder is retired before a worker builds from the new source bytes.
			source.Changed = true;
		}
		assets::MeshData mesh;
		if (provided != nullptr) {
			mesh = *provided;
			source.UsesProvidedMesh = true;
		} else {
			source.UsesProvidedMesh = false;
			const MeshCopyStatus status = ResolveSource(store, renderer, source, owner, mesh);
			if (status != MeshCopyStatus::Copied) {
				// A source that cannot be copied must not leave its prior ladder drawn.
				// Keeping Changed true retries transient admission failures next refresh.
				InvalidateSource(store, renderer, source, owner);
				source.Requested.clear();
				return 0;
			}
		}
		// Rebuilds replace source-derived bytes, never leave an old ladder visible
		// until the worker finishes. This also frees requests removed by a property edit.
		InvalidateSource(store, renderer, source, owner);
		scene::RecordMesh(store, source.Base, static_cast<uint32_t>(mesh.Indices.size() / 3));
		source.Requested = wanted;
		uint64_t revision = 0;
		if (source.UsesProvidedMesh) {
			source.SourceRevision = source.Generation;
		} else if (SourceRevision(store, renderer, source, owner, revision)) {
			source.SourceRevision = revision;
		} else {
			source.SourceRevision = 0;
		}
		const uint64_t generation = source.Generation;
		source.Changed = false;
		const uint64_t ticket = source.Next ? source.Next->Ticket : ++NextTicket;
		source.Next = std::make_unique<Queued>(Queued{
			ticket,
			generation,
			source.SourceRevision,
			std::move(mesh),
			wanted,
			std::vector<uint32_t>(welds.begin(), welds.end()),
			source.UsesProvidedMesh && source.Base.Text().starts_with("editable-mesh://"),
		});
		if (Active && Active->World == store.Identity() && Active->Owner == owner &&
			Active->Base == source.Base)
			Active->RequestCancel();
		return 0;
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

	bool
	AutomaticMeshLodUploader::RetainsArtifact(uint64_t world, core::Name owner, core::Name artifact) const {
		return std::any_of(Scopes.begin(), Scopes.end(), [&](const Scope &scope) {
			if (scope.World != world || scope.Owner != owner) return false;
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
			if (!RetainsArtifact(store.Identity(), owner, artifact)) (void)scene::ForgetMesh(store, artifact);
		}
	}

	void AutomaticMeshLodUploader::ForgetWorld(uint64_t identity) {
		if (Active && Active->World == identity) Active->RequestCancel();
		std::erase_if(Completed, [identity](const auto &job) { return job->World == identity; });
		std::erase_if(Scopes, [identity](const Scope &scope) { return scope.World == identity; });
	}

	void AutomaticMeshLodUploader::ForgetOwner(core::Name owner) {
		if (Active && Active->Owner == owner) Active->RequestCancel();
		std::erase_if(Completed, [owner](const auto &job) { return job->Owner == owner; });
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
