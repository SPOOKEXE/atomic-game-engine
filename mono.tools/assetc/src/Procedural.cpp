#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/procedural/Geometry.hpp>

#include <algorithm>
#include <assetc/ImageGraph.hpp>
#include <assetc/Procedural.hpp>
#include <cstddef>
#include <fstream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace assetc {
	namespace procedural_fs = std::filesystem;
	namespace procedural_assetc_detail {
		constexpr size_t MAXIMUM_MESH_BYTES = 40 * 1024 * 1024;

		bool RefuseProcedural(std::string &failure, const char *reason) {
			failure = reason;
			return false;
		}

		bool WithinProceduralRoot(const procedural_fs::path &root, const procedural_fs::path &path) {
			return std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first == root.end();
		}

		bool ValidProceduralRelativeName(std::string_view name, std::string_view extension) {
			if (name.empty() || name.size() > 512 || name.find('\\') != std::string_view::npos ||
				name.find(':') != std::string_view::npos || name.find('\0') != std::string_view::npos)
				return false;
			const procedural_fs::path path{std::string(name)};
			if (path.is_absolute() || path.extension().generic_string() != extension ||
				path.lexically_normal() != path)
				return false;
			for (const procedural_fs::path &component : path)
				if (component.empty() || component == "." || component == "..") return false;
			return true;
		}

		bool ConfinedProceduralPath(
			const procedural_fs::path &root,
			std::string_view name,
			procedural_fs::path &out,
			std::string &failure
		) {
			std::error_code error;
			const procedural_fs::path path = procedural_fs::weakly_canonical(root / std::string(name), error);
			if (error || !WithinProceduralRoot(root, path))
				return RefuseProcedural(failure, "procedural bake: output path leaves root");
			out = path;
			return true;
		}

		bool ReadProceduralFile(
			const procedural_fs::path &path, size_t maximum, std::vector<std::byte> &out, std::string &failure
		) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) return RefuseProcedural(failure, "procedural bake: source unavailable");
			const std::streamoff length = file.tellg();
			if (length < 0 || static_cast<uint64_t>(length) > maximum)
				return RefuseProcedural(failure, "procedural bake: source byte limit");
			file.seekg(0, std::ios::beg);
			std::vector<std::byte> candidate(static_cast<size_t>(length));
			file.read(reinterpret_cast<char *>(candidate.data()), length);
			if (file.gcount() != length || file.bad())
				return RefuseProcedural(failure, "procedural bake: source read failed");
			out = std::move(candidate);
			return true;
		}

		bool
		SameProceduralScope(engine::procedural::RegionScope left, engine::procedural::RegionScope right) {
			return left.X == right.X && left.Y == right.Y && left.CellsPerSide == right.CellsPerSide;
		}
	}

	bool CookProceduralArtifact(
		std::span<const uint8_t> artifactBytes,
		engine::procedural::RegionScope expectedScope,
		engine::bake::ProceduralBake &out,
		std::string &failure
	) {
		using namespace engine;
		using namespace procedural_assetc_detail;
		procedural::RegionResult result;
		procedural::Diagnostic diagnostic;
		if (procedural::DecodeRegionResult(artifactBytes, result, diagnostic) != procedural::Status::Ok) {
			failure = "procedural bake: invalid RGA1 artifact";
			if (!diagnostic.Field.empty()) failure += " (" + diagnostic.Field + ")";
			return false;
		}
		procedural::RegionScope carriedScope;
		if (!procedural::ReadRegionKeyScope(result.KeyBytes, carriedScope) ||
			!SameProceduralScope(carriedScope, expectedScope))
			return RefuseProcedural(failure, "procedural bake: artifact scope does not match");
		procedural::RegionGeometry geometry;
		if (procedural::BuildRegionGeometry(result, expectedScope, geometry, diagnostic) !=
			procedural::Status::Ok) {
			failure = "procedural bake: invalid regional geometry";
			if (!diagnostic.Field.empty()) failure += " (" + diagnostic.Field + ")";
			return false;
		}
		return bake::BakeProceduralGeometry(geometry, out, failure);
	}

	bool ReadProceduralArtifactFile(
		const procedural_fs::path &inputRoot,
		std::string_view sourceName,
		std::vector<uint8_t> &out,
		std::string &failure
	) {
		using namespace procedural_assetc_detail;
		if (!ValidProceduralRelativeName(sourceName, ".rga"))
			return RefuseProcedural(failure, "procedural bake: invalid source name");
		try {
			std::error_code error;
			const procedural_fs::path root = procedural_fs::canonical(inputRoot, error);
			if (error || !procedural_fs::is_directory(root, error))
				return RefuseProcedural(failure, "procedural bake: input root unavailable");
			const procedural_fs::path source =
				procedural_fs::canonical(root / std::string(sourceName), error);
			if (error || !WithinProceduralRoot(root, source) ||
				!procedural_fs::is_regular_file(source, error))
				return RefuseProcedural(failure, "procedural bake: source leaves input root");
			std::vector<std::byte> bytes;
			if (!ReadProceduralFile(
					source, engine::procedural::ArtifactLimits{}.MaximumBytes, bytes, failure
				))
				return false;
			std::vector<uint8_t> candidate(bytes.size());
			std::transform(bytes.begin(), bytes.end(), candidate.begin(), [](std::byte value) {
				return static_cast<uint8_t>(value);
			});
			out = std::move(candidate);
			failure.clear();
			return true;
		} catch (const std::bad_alloc &) {
			return RefuseProcedural(failure, "procedural bake: allocation failed");
		}
	}

	bool PublishProceduralBake(
		const procedural_fs::path &outputRoot,
		std::string_view descriptionName,
		const engine::bake::ProceduralBake &baked,
		std::string &failure
	) {
		using namespace engine;
		using namespace procedural_assetc_detail;
		if (!ValidProceduralRelativeName(descriptionName, ".aproc") ||
			baked.Meshes.size() > procedural::GeometryLimits::HARD_MAXIMUM_OBJECTS ||
			baked.DescriptionBytes.size() > assets::MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES)
			return RefuseProcedural(failure, "procedural bake: invalid output name or count");
		try {
			std::vector<std::byte> canonical;
			if (!assets::EncodeProceduralDescription(baked.Placements, canonical, failure) ||
				canonical != baked.DescriptionBytes)
				return RefuseProcedural(failure, "procedural bake: description does not match placements");
			std::map<std::string, const assets::ProceduralPlacement *> referenced;
			for (const auto &placement : baked.Placements) {
				const auto [found, inserted] = referenced.emplace(placement.MeshName, &placement);
				if (!inserted &&
					(found->second->Kind != placement.Kind || found->second->Material != placement.Material ||
					 found->second->Centre != placement.Centre || found->second->Size != placement.Size))
					return RefuseProcedural(
						failure, "procedural bake: shared mesh has inconsistent placement"
					);
			}
			std::map<std::string, const bake::ProceduralMeshAsset *> meshes;
			size_t totalMeshBytes = 0;
			for (const auto &mesh : baked.Meshes) {
				if (mesh.Bytes.empty() || totalMeshBytes > MAXIMUM_MESH_BYTES ||
					mesh.Bytes.size() > MAXIMUM_MESH_BYTES - totalMeshBytes)
					return RefuseProcedural(failure, "procedural bake: mesh byte limit");
				totalMeshBytes += mesh.Bytes.size();
				const std::string expected =
					"procedural/" + assets::Hasher::Of(mesh.Bytes).ToHex() + ".amesh";
				if (mesh.Name != expected || !meshes.emplace(mesh.Name, &mesh).second)
					return RefuseProcedural(failure, "procedural bake: invalid mesh address");
				const auto placement = referenced.find(mesh.Name);
				if (placement == referenced.end())
					return RefuseProcedural(failure, "procedural bake: unreferenced mesh");
				core::ByteReader reader(mesh.Bytes);
				assets::MeshData ordinary;
				if (!assets::Mesh::Read(reader, ordinary) || !reader.AtEnd() ||
					!assets::ValidateProceduralPlacementMesh(*placement->second, ordinary, failure))
					return RefuseProcedural(failure, "procedural bake: invalid ordinary mesh");
			}
			if (referenced.size() != meshes.size())
				return RefuseProcedural(failure, "procedural bake: missing referenced mesh");

			std::error_code error;
			procedural_fs::create_directories(outputRoot, error);
			const procedural_fs::path root = procedural_fs::canonical(outputRoot, error);
			if (error || !procedural_fs::is_directory(root, error))
				return RefuseProcedural(failure, "procedural bake: output root unavailable");
			procedural_fs::path descriptionPath;
			if (!ConfinedProceduralPath(root, descriptionName, descriptionPath, failure)) return false;
			std::map<std::string, procedural_fs::path> meshPaths;
			for (const auto &[name, mesh] : meshes) {
				procedural_fs::path path;
				if (!ConfinedProceduralPath(root, name, path, failure)) return false;
				meshPaths.emplace(name, path);
				if (procedural_fs::exists(path, error)) {
					std::vector<std::byte> previous;
					if (!ReadProceduralFile(path, mesh->Bytes.size(), previous, failure) ||
						previous != mesh->Bytes)
						return RefuseProcedural(
							failure, "procedural bake: mesh address holds different bytes"
						);
				}
				if (error) return RefuseProcedural(failure, "procedural bake: mesh path unavailable");
			}
			for (const auto &[name, mesh] : meshes) {
				const procedural_fs::path &path = meshPaths.at(name);
				if (procedural_fs::exists(path, error)) continue;
				if (error || !PublishImageGraphTexture(path, mesh->Bytes, failure)) return false;
			}
			if (!PublishImageGraphTexture(descriptionPath, baked.DescriptionBytes, failure)) return false;
			failure.clear();
			return true;
		} catch (const std::bad_alloc &) {
			return RefuseProcedural(failure, "procedural bake: allocation failed");
		} catch (const std::length_error &) {
			return RefuseProcedural(failure, "procedural bake: byte limit");
		}
	}
}
