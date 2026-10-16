#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/bake/Procedural.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::bake {

	namespace procedural_bake_detail {
		constexpr size_t MAXIMUM_MESH_BYTES = 40 * 1024 * 1024;
		constexpr int64_t MAXIMUM_ORIGIN = (static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1) *
										   64 * procedural::SUBCELLS_PER_CELL;
		constexpr std::string_view MESH_PREFIX = "procedural/";
		constexpr std::string_view MESH_SUFFIX = ".amesh";

		bool Refuse(std::string &failure, const char *reason) {
			failure = reason;
			return false;
		}

		bool Spend(size_t &used, size_t maximum, size_t amount) {
			if (used > maximum || amount > maximum - used) return false;
			used += amount;
			return true;
		}

		bool ValidKind(std::string_view kind) {
			return kind == "tree" || kind == "village-lot" || kind == "house" || kind == "entrance" ||
				   kind == "road";
		}

		bool ValidGeometry(const procedural::RegionGeometry &geometry, std::string &failure) {
			if (geometry.Objects.size() > procedural::GeometryLimits::HARD_MAXIMUM_OBJECTS ||
				geometry.VertexCount > procedural::GeometryLimits::HARD_MAXIMUM_VERTICES ||
				geometry.IndexCount > procedural::GeometryLimits::HARD_MAXIMUM_INDICES)
				return Refuse(failure, "procedural bake: geometry count limit");
			size_t vertices = 0;
			size_t indices = 0;
			size_t geometryBytes = 0;
			size_t meshBytes = 0;
			size_t descriptionBytes = 8;
			for (const procedural::GeometryObject &object : geometry.Objects) {
				if (!procedural::ValidStableName(object.Id) || object.Id.size() > 128 ||
					!ValidKind(object.Kind) || object.Origin.X < -MAXIMUM_ORIGIN ||
					object.Origin.X > MAXIMUM_ORIGIN || object.Origin.Y < -MAXIMUM_ORIGIN ||
					object.Origin.Y > MAXIMUM_ORIGIN)
					return Refuse(failure, "procedural bake: invalid object identity");
				for (size_t axis = 0; axis < 3; ++axis)
					if (!std::isfinite(object.Centre[axis]) || !std::isfinite(object.Size[axis]) ||
						object.Size[axis] < 0)
						return Refuse(failure, "procedural bake: invalid object bounds");
				for (float channel : object.Colour)
					if (!std::isfinite(channel) || channel < 0 || channel > 1)
						return Refuse(failure, "procedural bake: invalid object colour");
				if (!Spend(
						descriptionBytes,
						assets::MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES,
						72 + object.Id.size() + object.Kind.size() + MESH_PREFIX.size() + 64 +
							MESH_SUFFIX.size()
					))
					return Refuse(failure, "procedural bake: description byte limit");
				if (object.Vertices.empty() || object.Indices.empty() || object.Indices.size() % 3 != 0 ||
					!Spend(
						vertices, procedural::GeometryLimits::HARD_MAXIMUM_VERTICES, object.Vertices.size()
					) ||
					!Spend(indices, procedural::GeometryLimits::HARD_MAXIMUM_INDICES, object.Indices.size()))
					return Refuse(failure, "procedural bake: invalid geometry counts");
				if (!Spend(
						geometryBytes,
						procedural::GeometryLimits::HARD_MAXIMUM_BYTES,
						sizeof(procedural::GeometryObject) + object.Id.size() + object.Kind.size() +
							object.Vertices.size() * sizeof(procedural::GeometryVertex) +
							object.Indices.size() * sizeof(uint32_t)
					))
					return Refuse(failure, "procedural bake: geometry byte limit");
				const size_t size = 20 + object.Vertices.size() * sizeof(assets::MeshVertex) +
									object.Indices.size() * sizeof(uint32_t) + 8 + 8 + 16 + 128;
				if (!Spend(meshBytes, MAXIMUM_MESH_BYTES, size))
					return Refuse(failure, "procedural bake: mesh byte limit");
				for (uint32_t index : object.Indices)
					if (index >= object.Vertices.size())
						return Refuse(failure, "procedural bake: invalid mesh index");
				std::array<float, 3> minimum = object.Vertices.front().Position;
				std::array<float, 3> maximum = minimum;
				for (const procedural::GeometryVertex &vertex : object.Vertices) {
					for (size_t axis = 0; axis < 3; ++axis) {
						const float value = vertex.Position[axis];
						if (!std::isfinite(value))
							return Refuse(failure, "procedural bake: nonfinite vertex");
						minimum[axis] = std::min(minimum[axis], value);
						maximum[axis] = std::max(maximum[axis], value);
					}
					for (float value : vertex.Normal)
						if (!std::isfinite(value))
							return Refuse(failure, "procedural bake: nonfinite normal");
					for (float value : vertex.UV)
						if (!std::isfinite(value)) return Refuse(failure, "procedural bake: nonfinite UV");
				}
				for (size_t axis = 0; axis < 3; ++axis)
					if (object.Centre[axis] != (minimum[axis] + maximum[axis]) * 0.5f ||
						object.Size[axis] != maximum[axis] - minimum[axis])
						return Refuse(failure, "procedural bake: inconsistent object bounds");
			}
			if (vertices != geometry.VertexCount || indices != geometry.IndexCount)
				return Refuse(failure, "procedural bake: inconsistent totals");
			return true;
		}
	}

	bool BakeProceduralGeometry(
		const procedural::RegionGeometry &geometry, ProceduralBake &out, std::string &failure
	) {
		using namespace procedural_bake_detail;
		if (!ValidGeometry(geometry, failure)) return false;
		try {
			std::vector<const procedural::GeometryObject *> rows;
			rows.reserve(geometry.Objects.size());
			for (const procedural::GeometryObject &object : geometry.Objects)
				rows.push_back(&object);
			std::sort(rows.begin(), rows.end(), [](const auto *left, const auto *right) {
				return procedural::StableNameLess(left->Id, right->Id);
			});
			ProceduralBake baked;
			baked.Placements.reserve(rows.size());
			std::map<std::string, std::vector<std::byte>> uniqueMeshes;
			for (const procedural::GeometryObject *object : rows) {
				assets::MeshData mesh;
				mesh.Vertices.reserve(object->Vertices.size());
				for (const procedural::GeometryVertex &source : object->Vertices) {
					assets::MeshVertex vertex{};
					for (size_t axis = 0; axis < 3; ++axis) {
						vertex.Position[axis] = source.Position[axis];
						vertex.Normal[axis] = source.Normal[axis];
					}
					for (size_t axis = 0; axis < 2; ++axis)
						vertex.TexCoord[axis] = source.UV[axis];
					mesh.Vertices.push_back(vertex);
				}
				mesh.Indices = object->Indices;
				assets::Submesh run;
				run.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
				run.Material = "procedural/" + object->Kind + "/" + std::to_string(object->Material);
				// The placement applies palette colour as the MeshPart tint. A second
				// material colour would square it in the renderer's multiply path.
				mesh.Submeshes.push_back(std::move(run));
				if (!mesh.IsValid()) return Refuse(failure, "procedural bake: invalid ordinary mesh");
				mesh.ComputeBounds();
				core::ByteWriter writer(0, MAXIMUM_MESH_BYTES);
				if (!assets::Mesh::Write(writer, mesh))
					return Refuse(failure, "procedural bake: mesh serialization refused");
				std::vector<std::byte> bytes = writer.TakeBytes();
				const std::string name =
					std::string(MESH_PREFIX) + assets::Hasher::Of(bytes).ToHex() + std::string(MESH_SUFFIX);
				const auto existing = uniqueMeshes.find(name);
				if (existing != uniqueMeshes.end()) {
					if (existing->second != bytes)
						return Refuse(failure, "procedural bake: mesh hash collision");
				} else {
					uniqueMeshes.emplace(name, std::move(bytes));
				}
				baked.Placements.push_back(
					{object->Id,
					 object->Kind,
					 name,
					 object->Origin,
					 object->Material,
					 object->Variant,
					 object->Centre,
					 object->Size,
					 object->Colour}
				);
			}
			if (!assets::EncodeProceduralDescription(baked.Placements, baked.DescriptionBytes, failure))
				return false;
			baked.Meshes.reserve(uniqueMeshes.size());
			for (auto &[name, bytes] : uniqueMeshes)
				baked.Meshes.push_back({name, std::move(bytes)});
			out = std::move(baked);
			failure.clear();
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(failure, "procedural bake: allocation failed");
		} catch (const std::length_error &) {
			return Refuse(failure, "procedural bake: byte limit");
		}
	}
}
