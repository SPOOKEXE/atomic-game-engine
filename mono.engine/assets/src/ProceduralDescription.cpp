#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/procedural/Geometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace engine::assets {
	namespace procedural_description_detail {
		constexpr size_t MAXIMUM_BYTES = MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES;
		constexpr int64_t MAXIMUM_ORIGIN = (static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1) *
										   64 * procedural::SUBCELLS_PER_CELL;
		constexpr std::string_view MESH_PREFIX = "procedural/";
		constexpr std::string_view MESH_SUFFIX = ".amesh";

		bool Refuse(std::string &failure, const char *reason) {
			failure = reason;
			return false;
		}

		bool ValidKind(std::string_view kind) {
			return kind == "tree" || kind == "village-lot" || kind == "house" || kind == "entrance" ||
				   kind == "road";
		}

		bool ValidMeshName(std::string_view name) {
			if (name.size() != MESH_PREFIX.size() + 64 + MESH_SUFFIX.size() ||
				!name.starts_with(MESH_PREFIX) || !name.ends_with(MESH_SUFFIX))
				return false;
			const auto hash = ContentHash::FromHex(name.substr(MESH_PREFIX.size(), 64));
			return hash.has_value();
		}

		bool ValidPlacement(const ProceduralPlacement &placement) {
			if (!procedural::ValidStableName(placement.Id) || placement.Id.size() > 128 ||
				!ValidKind(placement.Kind) || !ValidMeshName(placement.MeshName) ||
				placement.Origin.X < -MAXIMUM_ORIGIN || placement.Origin.X > MAXIMUM_ORIGIN ||
				placement.Origin.Y < -MAXIMUM_ORIGIN || placement.Origin.Y > MAXIMUM_ORIGIN)
				return false;
			for (size_t axis = 0; axis < 3; ++axis)
				if (!std::isfinite(placement.Centre[axis]) || !std::isfinite(placement.Size[axis]) ||
					placement.Size[axis] < 0)
					return false;
			for (float channel : placement.Colour)
				if (!std::isfinite(channel) || channel < 0 || channel > 1) return false;
			return true;
		}
	}

	bool EncodeProceduralDescription(
		std::span<const ProceduralPlacement> placements, std::vector<std::byte> &out, std::string &failure
	) {
		using namespace procedural_description_detail;
		if (placements.size() > procedural::GeometryLimits::HARD_MAXIMUM_OBJECTS)
			return Refuse(failure, "procedural description: too many placements");
		try {
			size_t bytes = 8;
			for (size_t index = 0; index < placements.size(); ++index) {
				const ProceduralPlacement &placement = placements[index];
				if (!ValidPlacement(placement) ||
					(index != 0 && !procedural::StableNameLess(placements[index - 1].Id, placement.Id)))
					return Refuse(failure, "procedural description: invalid or unordered placement");
				const size_t amount =
					72 + placement.Id.size() + placement.Kind.size() + placement.MeshName.size();
				if (bytes > MAXIMUM_BYTES || amount > MAXIMUM_BYTES - bytes)
					return Refuse(failure, "procedural description: byte limit");
				bytes += amount;
			}
			core::ByteWriter writer(bytes, MAXIMUM_BYTES);
			writer.WriteRaw("PRD1", 4);
			writer.WriteUInt32(static_cast<uint32_t>(placements.size()));
			for (const ProceduralPlacement &placement : placements) {
				writer.WriteString(placement.Id);
				writer.WriteString(placement.Kind);
				writer.WriteString(placement.MeshName);
				writer.WriteInt64(placement.Origin.X);
				writer.WriteInt64(placement.Origin.Y);
				writer.WriteUInt16(placement.Material);
				writer.WriteUInt16(placement.Variant);
				for (float value : placement.Centre)
					writer.WriteFloat(value == 0 ? 0 : value);
				for (float value : placement.Size)
					writer.WriteFloat(value == 0 ? 0 : value);
				for (float value : placement.Colour)
					writer.WriteFloat(value == 0 ? 0 : value);
			}
			out = writer.TakeBytes();
			failure.clear();
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(failure, "procedural description: allocation failed");
		} catch (const std::length_error &) {
			return Refuse(failure, "procedural description: byte limit");
		}
	}

	bool DecodeProceduralDescription(
		std::span<const std::byte> bytes, std::vector<ProceduralPlacement> &out, std::string &failure
	) {
		using namespace procedural_description_detail;
		if (bytes.size() < 8 || bytes.size() > MAXIMUM_BYTES)
			return Refuse(failure, "procedural description: byte limit");
		try {
			core::ByteReader reader(bytes);
			for (char expected : std::string_view("PRD1"))
				if (reader.ReadUInt8() != static_cast<uint8_t>(expected))
					return Refuse(failure, "procedural description: magic");
			const uint32_t count = reader.ReadUInt32();
			if (reader.Failed() || count > procedural::GeometryLimits::HARD_MAXIMUM_OBJECTS ||
				count > reader.Remaining() / 72)
				return Refuse(failure, "procedural description: count");
			std::vector<ProceduralPlacement> parsed;
			parsed.reserve(count);
			for (uint32_t index = 0; index < count; ++index) {
				ProceduralPlacement placement;
				placement.Id = reader.ReadString();
				placement.Kind = reader.ReadString();
				placement.MeshName = reader.ReadString();
				placement.Origin.X = reader.ReadInt64();
				placement.Origin.Y = reader.ReadInt64();
				placement.Material = reader.ReadUInt16();
				placement.Variant = reader.ReadUInt16();
				for (float &value : placement.Centre)
					value = reader.ReadFloat();
				for (float &value : placement.Size)
					value = reader.ReadFloat();
				for (float &value : placement.Colour)
					value = reader.ReadFloat();
				if (reader.Failed() || !ValidPlacement(placement) ||
					(!parsed.empty() && !procedural::StableNameLess(parsed.back().Id, placement.Id)))
					return Refuse(failure, "procedural description: invalid placement");
				parsed.push_back(std::move(placement));
			}
			if (!reader.AtEnd()) return Refuse(failure, "procedural description: trailing bytes");
			std::vector<std::byte> canonical;
			if (!EncodeProceduralDescription(parsed, canonical, failure) ||
				canonical.size() != bytes.size() ||
				!std::equal(canonical.begin(), canonical.end(), bytes.begin()))
				return Refuse(failure, "procedural description: noncanonical bytes");
			out = std::move(parsed);
			failure.clear();
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(failure, "procedural description: allocation failed");
		} catch (const std::length_error &) {
			return Refuse(failure, "procedural description: byte limit");
		}
	}

	bool ValidateProceduralPlacementMesh(
		const ProceduralPlacement &placement, const MeshData &mesh, std::string &failure
	) {
		using namespace procedural_description_detail;
		if (!ValidPlacement(placement) || !mesh.IsValid() || mesh.JointCount != 0 ||
			mesh.Submeshes.size() != 1)
			return Refuse(failure, "procedural mesh: invalid placement or ordinary mesh");
		const Submesh &run = mesh.Submeshes.front();
		if (run.FirstIndex != 0 || run.IndexCount != mesh.Indices.size() || !run.Texture.empty() ||
			run.Material != "procedural/" + placement.Kind + "/" + std::to_string(placement.Material))
			return Refuse(failure, "procedural mesh: invalid material run");
		for (float channel : run.BaseColour)
			if (channel != 1.0f) return Refuse(failure, "procedural mesh: nonwhite material colour");
		std::array<float, 3> minimum{
			mesh.Vertices.front().Position[0],
			mesh.Vertices.front().Position[1],
			mesh.Vertices.front().Position[2]
		};
		std::array<float, 3> maximum = minimum;
		for (const MeshVertex &vertex : mesh.Vertices) {
			for (size_t slot = 0; slot < 4; ++slot)
				if (vertex.Joints[slot] != 0 || vertex.Weights[slot] != 0)
					return Refuse(failure, "procedural mesh: unexpected skinning");
			for (size_t axis = 0; axis < 3; ++axis) {
				minimum[axis] = std::min(minimum[axis], vertex.Position[axis]);
				maximum[axis] = std::max(maximum[axis], vertex.Position[axis]);
			}
		}
		for (size_t axis = 0; axis < 3; ++axis) {
			const float centre = (minimum[axis] + maximum[axis]) * 0.5f;
			const float size = maximum[axis] - minimum[axis];
			if (!std::isfinite(centre) || !std::isfinite(size) || placement.Centre[axis] != centre ||
				placement.Size[axis] != size)
				return Refuse(failure, "procedural mesh: placement bounds differ from vertices");
		}
		failure.clear();
		return true;
	}
}
