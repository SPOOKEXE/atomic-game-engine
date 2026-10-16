#include "ProceduralState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/procedural/Geometry.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <type_traits>

namespace engine::script {
	namespace procedural_scene_detail {
		constexpr size_t MAXIMUM_STAGING_BYTES = 64 * 1024 * 1024;
		constexpr size_t MAXIMUM_REGION_STAGING = procedural::GeometryLimits::HARD_MAXIMUM_BYTES +
												  procedural::GeometryLimits::HARD_MAXIMUM_VERTICES * 48ULL +
												  procedural::GeometryLimits::HARD_MAXIMUM_INDICES * 4ULL +
												  2 * 1024 * 1024;
		bool SameFrame(const core::CFrame &a, const core::CFrame &b) {
			return a.Position == b.Position && a.QuaternionX == b.QuaternionX &&
				   a.QuaternionY == b.QuaternionY && a.QuaternionZ == b.QuaternionZ &&
				   a.QuaternionW == b.QuaternionW;
		}
		bool Decoration(const scene::Collider &collider) {
			return !collider.CanQuery && collider.Mask == spatial::LayerMask::None();
		}
		bool TargetValid(const ecs::Store &store, const ProceduralSceneTarget &target) {
			if (target.StoreIdentity != store.Identity()) return false;
			const auto *mesh = store.Get<scene::EditableMesh>(target.Mesh);
			const auto *frame = store.Get<scene::Transform>(target.Part);
			const auto *previous = store.Get<scene::PreviousTransform>(target.Part);
			const auto *bounds = store.Get<scene::Bounds>(target.Part);
			const auto *visual = store.Get<scene::Visual>(target.Part);
			const auto *collider = store.Get<scene::Collider>(target.Part);
			return mesh && frame && previous && bounds && visual && collider &&
				   store.Get<ProceduralSceneRegion>(target.Mesh) &&
				   store.IsA(target.Part, ecs::Classes::Find(core::Name("MeshPart"))) &&
				   !store.Has<scene::Simulated>(target.Part) && mesh->Revision == target.MeshRevision &&
				   visual->Mesh == target.Content && visual->Fitted == target.Content &&
				   Decoration(*collider) && visual->Tint == core::Color3{1, 1, 1} &&
				   visual->Transparency == 0.0f && visual->Visible && SameFrame(frame->Frame, target.Frame) &&
				   SameFrame(previous->Frame, target.Previous) && bounds->HalfExtent == target.HalfExtent &&
				   scene::EditableMeshContentName(store, target.Mesh) == target.Content;
		}
		struct StagedRegion {
			scene::EditableMeshUpdate Mesh{};
			ProceduralSceneRegion Region{};
			core::CFrame Frame{};
			core::Vector3 HalfExtent{};
		};
		struct Destinations {
			scene::Transform *Frame = nullptr;
			scene::PreviousTransform *Previous = nullptr;
			scene::Bounds *Bounds = nullptr;
			ProceduralSceneRegion *Region = nullptr;
		};
		bool Materialize(
			ecs::Store &store,
			std::span<const ProceduralSceneTarget> targets,
			ProceduralPublished &candidate,
			bool publish,
			std::span<ProceduralCompletion *> accepted,
			procedural::Diagnostic &diagnostic
		) {
			using namespace procedural_detail;
			if (!store.IsOnOwningThread() || store.AdoptOnly() || targets.empty() ||
				targets.size() > ProceduralHostLimits::MaximumRetainedRequests)
				return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.owner_or_count");
			try {
				std::vector<StagedRegion> staged;
				staged.reserve(targets.size());
				size_t stagingBytes = 0;
				for (const auto &publication : candidate.Regions) {
					if (publication.ArtifactBytes.size() >
						ProceduralHostLimits::MaximumPublishedBytes - stagingBytes)
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "scene.artifacts");
					stagingBytes += publication.ArtifactBytes.size();
				}
				for (size_t index = 0; index < targets.size(); ++index) {
					const auto &target = targets[index];
					if (!TargetValid(store, target))
						return Refuse(diagnostic, procedural::Status::StaleArtifact, "scene.target");
					for (size_t prior = 0; prior < index; ++prior)
						if (target.Mesh == targets[prior].Mesh || target.Part == targets[prior].Part ||
							SameScope(target.Scope, targets[prior].Scope))
							return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.duplicate");
					const auto publication = std::find_if(
						candidate.Regions.begin(), candidate.Regions.end(), [&](const auto &row) {
							return SameScope(row.Scope, target.Scope);
						}
					);
					if (publication == candidate.Regions.end() ||
						Hash(publication->ArtifactBytes) != publication->ArtifactDigest)
						return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.artifact");
					if (stagingBytes > MAXIMUM_STAGING_BYTES - MAXIMUM_REGION_STAGING)
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "scene.staging");
					procedural::RegionResult result;
					if (procedural::DecodeRegionResult(publication->ArtifactBytes, result, diagnostic) !=
						procedural::Status::Ok)
						return false;
					if ((Hash(result.KeyBytes) != publication->GenerationKey ||
						 !KeyScopeMatches(result.KeyBytes, target.Scope)))
						return Refuse(diagnostic, procedural::Status::StaleArtifact, "scene.key");
					procedural::RegionGeometry geometry;
					if (procedural::BuildRegionGeometry(result, target.Scope, geometry, diagnostic) !=
						procedural::Status::Ok)
						return false;
					StagedRegion row;
					row.Mesh.Instance = target.Mesh;
					row.Mesh.ExpectedRevision = target.MeshRevision;
					row.Region.Scope = target.Scope;
					row.Region.ArtifactDigest = publication->ArtifactDigest;
					row.Region.Objects.reserve(geometry.Objects.size());
					scene::EditableMeshGeometry aggregate;
					aggregate.Positions.reserve(geometry.VertexCount);
					aggregate.Normals.reserve(geometry.VertexCount);
					aggregate.UVs.reserve(geometry.VertexCount);
					aggregate.Colours.reserve(geometry.VertexCount);
					aggregate.Alphas.reserve(geometry.VertexCount);
					aggregate.Indices.reserve(geometry.IndexCount);
					for (const auto &object : geometry.Objects) {
						const auto offset = static_cast<uint32_t>(aggregate.Positions.size());
						row.Region.Objects.push_back(
							{object.Id,
							 object.Kind,
							 static_cast<uint32_t>(aggregate.Indices.size()),
							 static_cast<uint32_t>(object.Indices.size()),
							 object.Material,
							 object.Variant}
						);
						for (const auto &vertex : object.Vertices) {
							const auto position = procedural::GeometryPositionRelativeTo(
								object, geometry.Origin, vertex.Position
							);
							aggregate.Positions.emplace_back(position[0], position[1], position[2]);
							aggregate.Normals.emplace_back(
								vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]
							);
							aggregate.UVs.emplace_back(vertex.UV[0], vertex.UV[1]);
							aggregate.Colours.emplace_back(
								object.Colour[0], object.Colour[1], object.Colour[2]
							);
							aggregate.Alphas.push_back(object.Colour[3]);
						}
						for (uint32_t vertex : object.Indices)
							aggregate.Indices.push_back(offset + vertex);
					}
					const auto centre = procedural::GeometryWorldCentre(geometry);
					row.Frame = core::CFrame{core::Vector3{centre[0], centre[1], centre[2]}};
					row.HalfExtent = core::Vector3{
						geometry.Size[0] * 0.5f, geometry.Size[1] * 0.5f, geometry.Size[2] * 0.5f
					};
					row.Mesh.Prepared = scene::PrepareEditableMesh(std::move(aggregate));
					if (!row.Mesh.Prepared.Valid)
						return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.geometry");
					stagingBytes += geometry.VertexCount * 48ULL + geometry.IndexCount * 4ULL +
									geometry.Objects.size() * 384ULL;
					staged.push_back(std::move(row));
				}
				std::vector<scene::EditableMeshUpdate> updates;
				updates.reserve(staged.size());
				std::array<Destinations, ProceduralHostLimits::MaximumRetainedRequests> destinations{};
				for (size_t index = 0; index < targets.size(); ++index) {
					updates.push_back(std::move(staged[index].Mesh));
					destinations[index] = {
						store.GetMutable<scene::Transform>(targets[index].Part),
						store.GetMutable<scene::PreviousTransform>(targets[index].Part),
						store.GetMutable<scene::Bounds>(targets[index].Part),
						store.GetMutable<ProceduralSceneRegion>(targets[index].Mesh)
					};
				}
				auto *canonical = publish ? store.ResourceMutable<ProceduralPublished>() : nullptr;
				core::Metrics::Count("procedural.scene_output_bytes", static_cast<double>(stagingBytes));
				// Scene obtains all mesh destinations before its first write and performs
				// no structural operation, so the owner-local pointers above stay valid.
				const auto committed = scene::CommitEditableMeshBatch(store, updates);
				if (committed != scene::EditableMeshCommit::Applied &&
					committed != scene::EditableMeshCommit::Unchanged)
					return Refuse(diagnostic, procedural::Status::StaleArtifact, "scene.commit");
				static_assert(std::is_nothrow_copy_assignable_v<core::CFrame>);
				static_assert(std::is_nothrow_swappable_v<decltype(ProceduralSceneRegion::Objects)>);
				for (size_t index = 0; index < staged.size(); ++index) {
					auto &destination = destinations[index];
					auto &row = staged[index];
					destination.Frame->Frame = row.Frame;
					destination.Previous->Frame = row.Frame;
					destination.Bounds->HalfExtent = row.HalfExtent;
					destination.Region->Scope = row.Region.Scope;
					destination.Region->ArtifactDigest = row.Region.ArtifactDigest;
					destination.Region->Objects.swap(row.Region.Objects);
				}
				static_assert(std::is_nothrow_swappable_v<decltype(candidate.Regions)>);
				if (canonical) canonical->Regions.swap(candidate.Regions);
				for (auto *completion : accepted)
					completion->Status = ProceduralJobStatus::Published;
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "scene.bytes");
			}
		}
	} // namespace procedural_scene_detail
	void RegisterProceduralSceneComponents() {
		ecs::Components::Register<ProceduralSceneRegion>(
			"script.ProceduralSceneRegion",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *rows = static_cast<ProceduralSceneRegion *>(destination);
				for (size_t i = 0; i < count; ++i)
					rows[i] = {};
			},
			0
		);
	}
	bool BindProceduralSceneTarget(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		ProceduralSceneTarget &out,
		procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		if (!store.IsOnOwningThread() || store.AdoptOnly())
			return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.owner");
		const auto *held = store.Get<scene::EditableMesh>(mesh);
		const auto *frame = store.Get<scene::Transform>(part);
		const auto *previous = store.Get<scene::PreviousTransform>(part);
		const auto *bounds = store.Get<scene::Bounds>(part);
		const auto *visual = store.Get<scene::Visual>(part);
		const auto *collider = store.Get<scene::Collider>(part);
		const auto content = scene::EditableMeshContentName(store, mesh);
		if (!held || !frame || !previous || !bounds || !visual || !collider || !content.IsValid() ||
			!store.IsA(part, ecs::Classes::Find(core::Name("MeshPart"))) || visual->Mesh != content ||
			store.Has<scene::Simulated>(part) || !procedural_scene_detail::Decoration(*collider) ||
			visual->Tint != core::Color3{1, 1, 1} || visual->Transparency != 0.0f || !visual->Visible)
			return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.binding");
		const ProceduralSceneTarget candidate{
			scope,
			mesh,
			part,
			store.Identity(),
			held->Revision,
			content,
			frame->Frame,
			previous->Frame,
			bounds->HalfExtent
		};
		if (!store.Has<ProceduralSceneRegion>(mesh)) store.Set(mesh, ProceduralSceneRegion{});
		store.GetMutable<scene::Visual>(part)->Fitted = content;
		out = candidate;
		diagnostic = {};
		return true;
	}
	bool ProceduralJobs::PublishSceneBatch(
		ecs::Store &store,
		std::span<const uint64_t> tickets,
		std::span<const ProceduralSceneTarget> targets,
		uint64_t activationTick
	) {
		ENGINE_PROFILE("Procedural::PublishScene");
		procedural_detail::PublicationCandidate candidate;
		if (!PreparePublication(store, tickets, activationTick, candidate)) return false;
		if (targets.size() != candidate.Accepted.size())
			return procedural_detail::Refuse(
				Held->Diagnostic, procedural::Status::InvalidInput, "scene.targets"
			);
		for (const auto &target : targets)
			if (std::none_of(
					candidate.Accepted.begin(), candidate.Accepted.end(), [&](const auto *completion) {
						return procedural_detail::SameScope(target.Scope, completion->Scope);
					}
				))
				return procedural_detail::Refuse(
					Held->Diagnostic, procedural::Status::InvalidInput, "scene.scope"
				);
		return procedural_scene_detail::Materialize(
			store, targets, candidate.Published, true, candidate.Accepted, Held->Diagnostic
		);
	}
	bool MaterializePublishedProceduralRegions(
		ecs::Store &store, std::span<const ProceduralSceneTarget> targets, procedural::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("Procedural::LoadPublishedScene");
		if (!store.IsOnOwningThread())
			return procedural_detail::Refuse(diagnostic, procedural::Status::InvalidInput, "scene.owner");
		const auto *published = store.Resource<ProceduralPublished>();
		if (!published)
			return procedural_detail::Refuse(
				diagnostic, procedural::Status::MissingSource, "scene.publications"
			);
		if (published->Regions.size() > ProceduralHostLimits::MaximumRegions)
			return procedural_detail::Refuse(diagnostic, procedural::Status::LimitExceeded, "scene.regions");
		size_t artifactBytes = 0;
		for (const auto &publication : published->Regions) {
			if (publication.ArtifactBytes.size() >
				ProceduralHostLimits::MaximumPublishedBytes - artifactBytes)
				return procedural_detail::Refuse(
					diagnostic, procedural::Status::LimitExceeded, "scene.artifacts"
				);
			artifactBytes += publication.ArtifactBytes.size();
		}
		try {
			auto candidate = *published;
			return procedural_scene_detail::Materialize(store, targets, candidate, false, {}, diagnostic);
		} catch (const std::bad_alloc &) {
			return procedural_detail::Refuse(diagnostic, procedural::Status::AllocationFailed, "scene.bytes");
		}
	}
} // namespace engine::script
