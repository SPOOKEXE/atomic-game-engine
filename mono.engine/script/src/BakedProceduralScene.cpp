#include "ProceduralSceneState.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/BakedProceduralScene.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {
	namespace baked_procedural_detail {
		constexpr size_t MAXIMUM_MESH_BYTES = 40 * 1024 * 1024;
		constexpr size_t MAXIMUM_STAGING_BYTES = 96 * 1024 * 1024;
		constexpr std::string_view MESH_PREFIX = "procedural/";
		constexpr std::string_view MESH_SUFFIX = ".amesh";

		bool Refuse(procedural::Diagnostic &diagnostic, procedural::Status status, std::string_view field) {
			diagnostic.Code = status;
			diagnostic.Field.assign(field);
			return false;
		}

		bool Spend(size_t &used, size_t maximum, size_t amount) {
			if (amount > maximum - used) return false;
			used += amount;
			return true;
		}

		bool MeshNameHash(std::string_view name, assets::ContentHash &out) {
			if (name.size() != MESH_PREFIX.size() + 64 + MESH_SUFFIX.size() ||
				!name.starts_with(MESH_PREFIX) || !name.ends_with(MESH_SUFFIX))
				return false;
			const auto hash = assets::ContentHash::FromHex(name.substr(MESH_PREFIX.size(), 64));
			if (!hash || hash->IsZero()) return false;
			out = *hash;
			return true;
		}

		bool ScopeBounds(procedural::RegionScope scope, procedural::Bounds &owner) {
			if (scope.CellsPerSide == 0 || scope.CellsPerSide > 64) return false;
			const int64_t x = int64_t{scope.X} * scope.CellsPerSide * procedural::SUBCELLS_PER_CELL;
			const int64_t y = int64_t{scope.Y} * scope.CellsPerSide * procedural::SUBCELLS_PER_CELL;
			const int64_t side = int64_t{scope.CellsPerSide} * procedural::SUBCELLS_PER_CELL;
			owner = {{x, y}, {x + side, y + side}};
			return true;
		}

		bool Contains(procedural::Bounds owner, procedural::Point point) {
			return point.X >= owner.Min.X && point.X < owner.Max.X && point.Y >= owner.Min.Y &&
				   point.Y < owner.Max.Y;
		}

		struct DecodedMesh {
			assets::MeshData Data{};
		};

		bool ValidateMesh(
			const BakedProceduralMesh &source, DecodedMesh &out, procedural::Diagnostic &diagnostic
		) {
			assets::ContentHash expected;
			if (!MeshNameHash(source.Name, expected) || source.Bytes.size() > MAXIMUM_MESH_BYTES ||
				assets::Hasher::Of(source.Bytes) != expected)
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.mesh_hash");
			core::ByteReader header(source.Bytes);
			if (header.ReadUInt32() != assets::Mesh::MAGIC || header.ReadUInt16() != assets::Mesh::VERSION)
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.mesh_version");
			core::ByteReader reader(source.Bytes);
			assets::MeshData parsed;
			if (!assets::Mesh::Read(reader, parsed) || !reader.AtEnd())
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.mesh_shape");
			out.Data = std::move(parsed);
			return true;
		}

		bool Stage(
			const BakedProceduralRegion &input,
			procedural_scene_detail::StagedRegion &staged,
			size_t &retainedBytes,
			procedural::Diagnostic &diagnostic
		) {
			ENGINE_PROFILE("Procedural::StageBakedScene");
			const auto &target = input.Target;
			const auto expectedDescriptionHash = input.DescriptionHash;
			const auto descriptionBytes = input.DescriptionBytes;
			const auto meshes = input.Meshes;
			if (expectedDescriptionHash.IsZero() ||
				descriptionBytes.size() > assets::MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES ||
				assets::Hasher::Of(descriptionBytes) != expectedDescriptionHash)
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.description_hash");
			if (meshes.size() > procedural::GeometryLimits::HARD_MAXIMUM_OBJECTS)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.mesh_count");
			procedural::Bounds owner;
			if (!ScopeBounds(target.Scope, owner))
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.scope");
			try {
				std::vector<assets::ProceduralPlacement> placements;
				std::string failure;
				if (!assets::DecodeProceduralDescription(descriptionBytes, placements, failure))
					return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.description");
				size_t payloadBytes = 0;
				for (size_t index = 0; index < meshes.size(); ++index) {
					if (index && !procedural::StableNameLess(meshes[index - 1].Name, meshes[index].Name))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.mesh_order");
					if (!Spend(payloadBytes, MAXIMUM_MESH_BYTES, meshes[index].Bytes.size()))
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.mesh_bytes");
				}
				std::vector<uint8_t> referenced(meshes.size());
				for (const auto &placement : placements) {
					if (!Contains(owner, placement.Origin))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.origin");
					const auto found = std::lower_bound(
						meshes.begin(),
						meshes.end(),
						placement.MeshName,
						[](const BakedProceduralMesh &mesh, std::string_view name) {
							return procedural::StableNameLess(mesh.Name, name);
						}
					);
					if (found == meshes.end() || found->Name != placement.MeshName)
						return Refuse(diagnostic, procedural::Status::MissingSource, "baked.mesh_missing");
					referenced[static_cast<size_t>(found - meshes.begin())] = 1;
				}
				if (std::find(referenced.begin(), referenced.end(), uint8_t{0}) != referenced.end())
					return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.mesh_extra");
				std::vector<DecodedMesh> decoded(meshes.size());
				size_t decodedBytes = 0;
				for (size_t index = 0; index < meshes.size(); ++index) {
					if (!ValidateMesh(meshes[index], decoded[index], diagnostic)) return false;
					const auto &mesh = decoded[index].Data;
					const size_t amount = mesh.Vertices.size() * sizeof(assets::MeshVertex) +
										  mesh.Indices.size() * sizeof(uint32_t) + sizeof(assets::Submesh);
					if (!Spend(decodedBytes, MAXIMUM_STAGING_BYTES, amount))
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.decoded_bytes");
				}
				procedural::RegionGeometry geometry;
				geometry.Origin = owner.Min;
				geometry.Objects.reserve(placements.size());
				size_t geometryBytes = 0, vertices = 0, indices = 0;
				ENGINE_PROFILE("Procedural::BuildBakedGeometry");
				for (const auto &placement : placements) {
					const auto found = std::lower_bound(
						meshes.begin(),
						meshes.end(),
						placement.MeshName,
						[](const BakedProceduralMesh &mesh, std::string_view name) {
							return procedural::StableNameLess(mesh.Name, name);
						}
					);
					const auto &mesh = decoded[static_cast<size_t>(found - meshes.begin())].Data;
					if (!assets::ValidateProceduralPlacementMesh(placement, mesh, failure))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.placement_mesh");
					if (!Spend(
							vertices, procedural::GeometryLimits::HARD_MAXIMUM_VERTICES, mesh.Vertices.size()
						) ||
						!Spend(
							indices, procedural::GeometryLimits::HARD_MAXIMUM_INDICES, mesh.Indices.size()
						))
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.geometry_count");
					const size_t amount = sizeof(procedural::GeometryObject) + placement.Id.size() +
										  placement.Kind.size() +
										  mesh.Vertices.size() * sizeof(procedural::GeometryVertex) +
										  mesh.Indices.size() * sizeof(uint32_t);
					if (!Spend(geometryBytes, procedural::GeometryLimits::HARD_MAXIMUM_BYTES, amount))
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.geometry_bytes");
					procedural::GeometryObject object;
					object.Id = placement.Id;
					object.Kind = placement.Kind;
					object.Origin = placement.Origin;
					object.Material = placement.Material;
					object.Variant = placement.Variant;
					object.Colour = placement.Colour;
					object.Centre = placement.Centre;
					object.Size = placement.Size;
					object.Vertices.reserve(mesh.Vertices.size());
					for (const auto &vertex : mesh.Vertices)
						object.Vertices.push_back(
							{{vertex.Position[0], vertex.Position[1], vertex.Position[2]},
							 {vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]},
							 {vertex.TexCoord[0], vertex.TexCoord[1]}}
						);
					object.Indices = mesh.Indices;
					geometry.Objects.push_back(std::move(object));
				}
				geometry.VertexCount = static_cast<uint32_t>(vertices);
				geometry.IndexCount = static_cast<uint32_t>(indices);
				const size_t aggregateBytes = vertices * 52ULL + indices * sizeof(uint32_t) +
											  placements.size() * sizeof(ProceduralObjectRange);
				if (decodedBytes > MAXIMUM_STAGING_BYTES - geometryBytes ||
					aggregateBytes > MAXIMUM_STAGING_BYTES - decodedBytes - geometryBytes)
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.staging");
				const size_t regionBytes = decodedBytes + geometryBytes + aggregateBytes;
				if (!Spend(retainedBytes, MAXIMUM_STAGING_BYTES, regionBytes))
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.batch_staging");
				procedural_scene_detail::StagedRegion candidate;
				candidate.Mesh.Instance = target.Mesh;
				candidate.Mesh.ExpectedRevision = target.MeshRevision;
				candidate.Region.Scope = target.Scope;
				candidate.Region.ArtifactDigest = {expectedDescriptionHash.Digest};
				if (!procedural_scene_detail::PackRegionGeometry(geometry, candidate, diagnostic))
					return false;
				staged = std::move(candidate);
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "baked.storage");
			} catch (const std::length_error &) {
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.storage");
			}
		}
	}

	bool InstallBakedProceduralSceneBatch(
		ecs::Store &store, std::span<const BakedProceduralRegion> regions, procedural::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("Procedural::InstallBakedScene");
		if (!store.IsOnOwningThread() || store.AdoptOnly() || regions.empty() ||
			regions.size() > ProceduralHostLimits::MaximumRetainedRequests)
			return baked_procedural_detail::Refuse(
				diagnostic, procedural::Status::InvalidInput, "baked.batch"
			);
		try {
			std::vector<ProceduralSceneTarget> targets;
			std::vector<procedural_scene_detail::StagedRegion> staged;
			targets.reserve(regions.size());
			staged.reserve(regions.size());
			size_t retainedBytes = 0;
			for (const auto &region : regions) {
				if (!procedural_scene_detail::CapturedTargetValid(store, region.Target))
					return baked_procedural_detail::Refuse(
						diagnostic, procedural::Status::StaleArtifact, "baked.target"
					);
				procedural_scene_detail::StagedRegion row;
				if (!baked_procedural_detail::Stage(region, row, retainedBytes, diagnostic)) return false;
				targets.push_back(region.Target);
				staged.push_back(std::move(row));
			}
			return procedural_scene_detail::CommitRegions(store, targets, staged, nullptr, {}, diagnostic);
		} catch (const std::bad_alloc &) {
			return baked_procedural_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "baked.batch_storage"
			);
		}
	}

	bool InstallBakedProceduralScene(
		ecs::Store &store,
		const ProceduralSceneTarget &target,
		assets::ContentHash expectedDescriptionHash,
		std::span<const std::byte> descriptionBytes,
		std::span<const BakedProceduralMesh> meshes,
		procedural::Diagnostic &diagnostic
	) {
		const BakedProceduralRegion region{target, expectedDescriptionHash, descriptionBytes, meshes};
		return InstallBakedProceduralSceneBatch(store, std::span(&region, 1), diagnostic);
	}
}
