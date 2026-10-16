#include "ProceduralSceneState.hpp"
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
#include <cmath>
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
		bool CapturedTargetValid(const ecs::Store &store, const ProceduralSceneTarget &target) {
			ProceduralSceneTarget current;
			procedural::Diagnostic diagnostic;
			return CaptureProceduralSceneTarget(
					   store, target.Scope, target.Mesh, target.Part, current, diagnostic
				   ) &&
				   current.StoreIdentity == target.StoreIdentity &&
				   current.MeshRevision == target.MeshRevision && current.Content == target.Content &&
				   current.BeforeFitted == target.BeforeFitted && current.BeforeMask == target.BeforeMask &&
				   SameFrame(current.Frame, target.Frame) && SameFrame(current.Previous, target.Previous) &&
				   current.HalfExtent == target.HalfExtent;
		}
		bool TargetValid(const ecs::Store &store, const ProceduralSceneTarget &target) {
			return CapturedTargetValid(store, target) && store.Get<ProceduralSceneRegion>(target.Mesh) &&
				   target.BeforeFitted == target.Content && target.BeforeMask == spatial::LayerMask::None();
		}
		struct Destinations {
			scene::Transform *Frame = nullptr;
			scene::PreviousTransform *Previous = nullptr;
			scene::Bounds *Bounds = nullptr;
			ProceduralSceneRegion *Region = nullptr;
			scene::Visual *Visual = nullptr;
			scene::Collider *Collider = nullptr;
		};
		bool PackRegionGeometry(
			const procedural::RegionGeometry &geometry, StagedRegion &out, procedural::Diagnostic &diagnostic
		) {
			using namespace procedural_detail;
			try {
				size_t vertexCount = 0, indexCount = 0;
				if (geometry.Objects.size() > procedural::GeometryLimits::HARD_MAXIMUM_OBJECTS)
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "scene.objects");
				for (const auto &object : geometry.Objects) {
					if (object.Id.size() > 128 || object.Kind.size() > 128 ||
						object.Vertices.size() >
							procedural::GeometryLimits::HARD_MAXIMUM_VERTICES - vertexCount ||
						object.Indices.size() > procedural::GeometryLimits::HARD_MAXIMUM_INDICES - indexCount)
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "scene.geometry");
					vertexCount += object.Vertices.size();
					indexCount += object.Indices.size();
				}
				if (vertexCount != geometry.VertexCount || indexCount != geometry.IndexCount)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.counts");
				StagedRegion row;
				row.Mesh.Instance = out.Mesh.Instance;
				row.Mesh.ExpectedRevision = out.Mesh.ExpectedRevision;
				row.Region.Scope = out.Region.Scope;
				row.Region.ArtifactDigest = out.Region.ArtifactDigest;
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
						const auto position =
							procedural::GeometryPositionRelativeTo(object, geometry.Origin, vertex.Position);
						aggregate.Positions.emplace_back(position[0], position[1], position[2]);
						aggregate.Normals.emplace_back(vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]);
						aggregate.UVs.emplace_back(vertex.UV[0], vertex.UV[1]);
						aggregate.Colours.emplace_back(object.Colour[0], object.Colour[1], object.Colour[2]);
						aggregate.Alphas.push_back(object.Colour[3]);
					}
					for (uint32_t vertex : object.Indices) {
						if (vertex >= object.Vertices.size())
							return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.index");
						aggregate.Indices.push_back(offset + vertex);
					}
				}
				std::array<float, 3> minimum{}, maximum{};
				if (!aggregate.Positions.empty()) {
					minimum = {
						aggregate.Positions.front().X,
						aggregate.Positions.front().Y,
						aggregate.Positions.front().Z
					};
					maximum = minimum;
					for (const auto &position : aggregate.Positions) {
						const std::array<float, 3> coordinates{position.X, position.Y, position.Z};
						for (size_t axis = 0; axis < 3; ++axis) {
							if (!std::isfinite(coordinates[axis]))
								return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.position");
							minimum[axis] = std::min(minimum[axis], coordinates[axis]);
							maximum[axis] = std::max(maximum[axis], coordinates[axis]);
						}
					}
				}
				std::array<float, 3> localCentre{}, size{};
				for (size_t axis = 0; axis < 3; ++axis) {
					localCentre[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
					size[axis] = maximum[axis] - minimum[axis];
					if (!std::isfinite(localCentre[axis]) || !std::isfinite(size[axis]))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.bounds");
				}
				const auto centre = procedural::GeometryWorldCentre(geometry.Origin, localCentre);
				for (const float coordinate : centre)
					if (!std::isfinite(coordinate))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.centre");
				row.Frame = core::CFrame{core::Vector3{centre[0], centre[1], centre[2]}};
				row.HalfExtent = core::Vector3{size[0] * 0.5f, size[1] * 0.5f, size[2] * 0.5f};
				row.Mesh.Prepared = scene::PrepareEditableMesh(std::move(aggregate));
				if (!row.Mesh.Prepared.Valid)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.geometry");
				static_assert(std::is_nothrow_move_assignable_v<StagedRegion>);
				out = std::move(row);
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "scene.pack");
			}
		}

		bool CommitRegions(
			ecs::Store &store,
			std::span<const ProceduralSceneTarget> targets,
			std::span<StagedRegion> staged,
			ProceduralPublished *canonicalCandidate,
			std::span<ProceduralCompletion *> accepted,
			procedural::Diagnostic &diagnostic
		) {
			using namespace procedural_detail;
			if (!store.IsOnOwningThread() || store.AdoptOnly() || targets.empty() ||
				targets.size() != staged.size() ||
				targets.size() > ProceduralHostLimits::MaximumRetainedRequests)
				return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.commit_targets");
			for (size_t index = 0; index < targets.size(); ++index) {
				const auto &target = targets[index];
				const auto &row = staged[index];
				if (!CapturedTargetValid(store, target) || row.Mesh.Instance != target.Mesh ||
					row.Mesh.ExpectedRevision != target.MeshRevision ||
					!SameScope(row.Region.Scope, target.Scope))
					return Refuse(diagnostic, procedural::Status::StaleArtifact, "scene.commit_target");
				for (size_t prior = 0; prior < index; ++prior)
					if (target.Mesh == targets[prior].Mesh || target.Part == targets[prior].Part ||
						SameScope(target.Scope, targets[prior].Scope))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.duplicate");
			}
			if (canonicalCandidate && !store.Resource<ProceduralPublished>())
				return Refuse(diagnostic, procedural::Status::MissingSource, "scene.canonical");
			try {
				std::vector<scene::EditableMeshUpdate> updates;
				updates.reserve(staged.size());
				std::array<Destinations, ProceduralHostLimits::MaximumRetainedRequests> destinations{};
				// Marker additions inherit Store::Set's structural exception guarantee.
				// Finish every structural operation before retaining any component pointer.
				for (const auto &target : targets)
					if (!store.Has<ProceduralSceneRegion>(target.Mesh))
						store.Set(target.Mesh, ProceduralSceneRegion{});
				for (size_t index = 0; index < targets.size(); ++index) {
					updates.push_back(std::move(staged[index].Mesh));
					destinations[index] = {
						store.GetMutable<scene::Transform>(targets[index].Part),
						store.GetMutable<scene::PreviousTransform>(targets[index].Part),
						store.GetMutable<scene::Bounds>(targets[index].Part),
						store.GetMutable<ProceduralSceneRegion>(targets[index].Mesh),
						store.GetMutable<scene::Visual>(targets[index].Part),
						store.GetMutable<scene::Collider>(targets[index].Part)
					};
				}
				auto *canonical = canonicalCandidate ? store.ResourceMutable<ProceduralPublished>() : nullptr;

				// Scene obtains all mesh destinations before its first write and performs
				// no structural operation, so the owner-local pointers above stay valid.
				const auto committed = scene::CommitEditableMeshBatch(store, updates);
				if (committed != scene::EditableMeshCommit::Applied &&
					committed != scene::EditableMeshCommit::Unchanged)
					return Refuse(diagnostic, procedural::Status::StaleArtifact, "scene.commit");
				static_assert(std::is_nothrow_copy_assignable_v<core::CFrame>);
				static_assert(std::is_nothrow_copy_assignable_v<core::Name>);
				static_assert(std::is_nothrow_copy_assignable_v<spatial::LayerMask>);
				static_assert(std::is_nothrow_swappable_v<decltype(ProceduralSceneRegion::Objects)>);
				for (size_t index = 0; index < staged.size(); ++index) {
					auto &destination = destinations[index];
					auto &row = staged[index];
					destination.Visual->Fitted = targets[index].Content;
					destination.Collider->Mask = spatial::LayerMask::None();
					destination.Frame->Frame = row.Frame;
					destination.Previous->Frame = row.Frame;
					destination.Bounds->HalfExtent = row.HalfExtent;
					destination.Region->Scope = row.Region.Scope;
					destination.Region->ArtifactDigest = row.Region.ArtifactDigest;
					destination.Region->Objects.swap(row.Region.Objects);
				}
				static_assert(std::is_nothrow_swappable_v<decltype(canonicalCandidate->Regions)>);
				if (canonical) canonical->Regions.swap(canonicalCandidate->Regions);
				for (auto *completion : accepted)
					completion->Status = ProceduralJobStatus::Published;
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "scene.commit");
			}
		}

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
					if (!PackRegionGeometry(geometry, row, diagnostic)) return false;
					stagingBytes += geometry.VertexCount * 48ULL + geometry.IndexCount * 4ULL +
									geometry.Objects.size() * 384ULL;
					staged.push_back(std::move(row));
				}
				core::Metrics::Count("procedural.scene_output_bytes", static_cast<double>(stagingBytes));
				return CommitRegions(
					store, targets, staged, publish ? &candidate : nullptr, accepted, diagnostic
				);
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
	bool procedural_scene_detail::CaptureTarget(
		const ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		core::Name expectedBinding,
		const scene::Collider &candidateCollider,
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
		if (mesh == part || !held || !frame || !previous || !bounds || !visual || !collider ||
			!content.IsValid() || !store.IsA(part, ecs::Classes::Find(core::Name("MeshPart"))) ||
			visual->Mesh != expectedBinding || store.Has<scene::Simulated>(part) ||
			(!candidateCollider.Trigger && candidateCollider.Mask != spatial::LayerMask::None()) ||
			candidateCollider.CanQuery || visual->Tint != core::Color3{1, 1, 1} ||
			visual->Transparency != 0.0f || !visual->Visible)
			return Refuse(diagnostic, procedural::Status::InvalidInput, "scene.binding");
		const ProceduralSceneTarget candidate{
			scope,
			mesh,
			part,
			store.Identity(),
			held->Revision,
			visual->Fitted,
			candidateCollider.Mask,
			content,
			frame->Frame,
			previous->Frame,
			bounds->HalfExtent
		};
		out = candidate;
		diagnostic = {};
		return true;
	}
	bool CaptureProceduralSceneTarget(
		const ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		ProceduralSceneTarget &out,
		procedural::Diagnostic &diagnostic
	) {
		if (!store.IsOnOwningThread() || store.AdoptOnly())
			return procedural_detail::Refuse(diagnostic, procedural::Status::InvalidInput, "scene.owner");
		const auto *collider = store.Get<scene::Collider>(part);
		if (!collider)
			return procedural_detail::Refuse(diagnostic, procedural::Status::InvalidInput, "scene.binding");
		return procedural_scene_detail::CaptureTarget(
			store, scope, mesh, part, scene::EditableMeshContentName(store, mesh), *collider, out, diagnostic
		);
	}
	namespace procedural_scene_detail {
		bool ProvisionTarget(
			ecs::Store &store, ProceduralSceneTarget &target, procedural::Diagnostic &diagnostic
		) {
			try {
				if (!CapturedTargetValid(store, target))
					return procedural_detail::Refuse(
						diagnostic, procedural::Status::StaleArtifact, "scene.binding_changed"
					);
				// Marker provisioning is structural and inherits Store::Set's exception
				// guarantee. No part scalar or existing geometry changes before it succeeds.
				if (!store.Has<ProceduralSceneRegion>(target.Mesh))
					store.Set(target.Mesh, ProceduralSceneRegion{});
				auto *collider = store.GetMutable<scene::Collider>(target.Part);
				auto *visual = store.GetMutable<scene::Visual>(target.Part);
				// Both potentially throwing mutable marks have completed before any scalar edit.
				collider->Mask = spatial::LayerMask::None();
				visual->Fitted = target.Content;
				target.BeforeFitted = target.Content;
				target.BeforeMask = spatial::LayerMask::None();
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return procedural_detail::Refuse(
					diagnostic, procedural::Status::AllocationFailed, "scene.provision"
				);
			}
		}
	}
	bool BindProceduralSceneTarget(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		ProceduralSceneTarget &out,
		procedural::Diagnostic &diagnostic
	) {
		ProceduralSceneTarget candidate;
		if (!CaptureProceduralSceneTarget(store, scope, mesh, part, candidate, diagnostic) ||
			!procedural_scene_detail::ProvisionTarget(store, candidate, diagnostic))
			return false;
		out = candidate;
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
