#include <engine/core/Bytes.hpp>
#include <engine/scene/CameraPortalTopology.hpp>

#include <cmath>
#include <unordered_set>

namespace engine::scene {
	namespace {
		constexpr uint32_t MAGIC = 0x31545043;
		constexpr size_t MAX_NAME = 256;
		bool Text(std::string_view text) {
			return !text.empty() && text.size() <= MAX_NAME && text.find('\0') == std::string_view::npos;
		}
		core::Vector3 Vector(const std::array<float, 3> &value) {
			return {value[0], value[1], value[2]};
		}
		template <size_t N> bool Finite(const std::array<float, N> &values) {
			for (float value : values)
				if (!std::isfinite(value)) return false;
			return true;
		}
		bool Valid(const CameraPortalMouth &mouth) {
			if (!Text(mouth.Name) || !Text(mouth.DestinationWorld) || !Finite(mouth.Centre) ||
				!Finite(mouth.Normal) || !Finite(mouth.First) || !Finite(mouth.Second) || !Finite(mouth.Up) ||
				!Finite(mouth.Destination) || !std::isfinite(mouth.Scale) || mouth.Scale <= 0)
				return false;
			const auto normal = Vector(mouth.Normal);
			const auto first = Vector(mouth.First);
			const auto second = Vector(mouth.Second);
			const auto up = Vector(mouth.Up);
			const double firstLength = std::hypot(double(first.X), double(first.Y), double(first.Z));
			const double secondLength = std::hypot(double(second.X), double(second.Y), double(second.Z));
			if (!(firstLength > 0 && secondLength > 0) || std::abs(normal.Dot(normal) - 1) > .001f)
				return false;
			const auto firstUnit = first / static_cast<float>(firstLength);
			const auto secondUnit = second / static_cast<float>(secondLength);
			if (!(first.Dot(first) > 0 && second.Dot(second) > 0) || !std::isfinite(first.Dot(first)) ||
				!std::isfinite(second.Dot(second)) || std::abs(firstUnit.Dot(secondUnit)) > .001f ||
				std::abs(firstUnit.Dot(normal)) > .001f || std::abs(secondUnit.Dot(normal)) > .001f)
				return false;
			const float upSquared = up.Dot(up);
			if (upSquared != 0 && (!std::isfinite(upSquared) || std::abs(upSquared - 1) > .001f ||
								   std::abs(up.Dot(normal)) > .001f))
				return false;
			double norm = 0;
			for (size_t index = 3; index < 7; ++index)
				norm += double(mouth.Destination[index]) * mouth.Destination[index];
			return std::abs(norm - 1) <= .001;
		}
		bool Valid(const CameraPortalTopology &topology) {
			if (!Text(topology.World) || topology.Revision == 0 ||
				topology.Mouths.size() > MAX_CAMERA_PORTAL_SEAMS)
				return false;
			std::unordered_set<std::string_view> names;
			for (const auto &mouth : topology.Mouths)
				if (!Valid(mouth) || mouth.DestinationWorld == topology.World ||
					!names.insert(mouth.Name).second)
					return false;
			return true;
		}
		bool Refuse(std::string &error) {
			error = "invalid camera portal topology";
			return false;
		}
		template <size_t N> void Write(core::ByteWriter &writer, const std::array<float, N> &values) {
			for (float value : values)
				writer.WriteFloat(value);
		}
		template <size_t N> void Read(core::ByteReader &reader, std::array<float, N> &values) {
			for (float &value : values)
				value = reader.ReadFloat();
		}
		std::string ReadText(core::ByteReader &reader) {
			const auto text = reader.ReadString();
			if (reader.Failed() || !Text(text)) {
				reader.Fail();
				return {};
			}
			return std::string(text);
		}
	}
	bool CopyCameraPortalMouth(
		std::string_view name, const PortalSeam &seam, CameraPortalMouth &out, std::string &error
	) {
		if (!seam.Crosses || !Text(name) || !seam.DestinationWorld.IsValid()) return Refuse(error);
		CameraPortalMouth mouth;
		mouth.Name = name;
		mouth.DestinationWorld = seam.DestinationWorld.Text();
		mouth.Centre = {seam.Centre.X, seam.Centre.Y, seam.Centre.Z};
		mouth.Normal = {seam.Normal.X, seam.Normal.Y, seam.Normal.Z};
		mouth.First = {seam.First.X, seam.First.Y, seam.First.Z};
		mouth.Second = {seam.Second.X, seam.Second.Y, seam.Second.Z};
		mouth.Up = {seam.Up.X, seam.Up.Y, seam.Up.Z};
		const auto &position = seam.Destination.Position;
		const auto rotation = seam.Destination.Rotation();
		mouth.Destination = {
			position.X, position.Y, position.Z, rotation.x, rotation.y, rotation.z, rotation.w
		};
		mouth.Scale = seam.Scale;
		mouth.Bidirectional = seam.Bidirectional;
		if (!Valid(mouth)) return Refuse(error);
		out = std::move(mouth);
		error.clear();
		return true;
	}
	bool EncodeCameraPortalTopology(
		const CameraPortalTopology &topology, std::vector<std::byte> &out, std::string &error
	) {
		if (!Valid(topology)) return Refuse(error);
		core::ByteWriter writer;
		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(1);
		writer.WriteUInt16(0);
		writer.WriteString(topology.World);
		writer.WriteUInt64(topology.Revision);
		writer.WriteUInt32(static_cast<uint32_t>(topology.Mouths.size()));
		for (const auto &mouth : topology.Mouths) {
			writer.WriteString(mouth.Name);
			writer.WriteString(mouth.DestinationWorld);
			Write(writer, mouth.Centre);
			Write(writer, mouth.Normal);
			Write(writer, mouth.First);
			Write(writer, mouth.Second);
			Write(writer, mouth.Up);
			Write(writer, mouth.Destination);
			writer.WriteFloat(mouth.Scale);
			writer.WriteUInt8(mouth.Bidirectional ? 1 : 0);
		}
		if (writer.Bytes().size() > MAX_CAMERA_PORTAL_TOPOLOGY_BYTES) return Refuse(error);
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		error.clear();
		return true;
	}
	bool DecodeCameraPortalTopology(
		std::span<const std::byte> bytes, CameraPortalTopology &out, std::string &error
	) {
		if (bytes.size() > MAX_CAMERA_PORTAL_TOPOLOGY_BYTES) return Refuse(error);
		core::ByteReader reader(bytes);
		if (reader.ReadUInt32() != MAGIC || reader.ReadUInt16() != 1 || reader.ReadUInt16() != 0)
			return Refuse(error);
		CameraPortalTopology topology;
		topology.World = ReadText(reader);
		topology.Revision = reader.ReadUInt64();
		const auto count = reader.ReadUInt32();
		// Each entry needs two nonempty strings, 23 floats and one policy byte.
		if (reader.Failed() || count > MAX_CAMERA_PORTAL_SEAMS || count > reader.Remaining() / 103)
			return Refuse(error);
		topology.Mouths.reserve(count);
		for (uint32_t index = 0; index < count; ++index) {
			CameraPortalMouth mouth;
			mouth.Name = ReadText(reader);
			mouth.DestinationWorld = ReadText(reader);
			Read(reader, mouth.Centre);
			Read(reader, mouth.Normal);
			Read(reader, mouth.First);
			Read(reader, mouth.Second);
			Read(reader, mouth.Up);
			Read(reader, mouth.Destination);
			mouth.Scale = reader.ReadFloat();
			const auto policy = reader.ReadUInt8();
			if (reader.Failed() || policy > 1) return Refuse(error);
			mouth.Bidirectional = policy == 1;
			topology.Mouths.push_back(std::move(mouth));
		}
		if (!reader.AtEnd() || !Valid(topology)) return Refuse(error);
		out = std::move(topology);
		error.clear();
		return true;
	}
	bool ResolveCameraPortalTopology(
		const CameraPortalTopology &topology, std::vector<PortalSeam> &out, std::string &error
	) {
		if (!Valid(topology)) return Refuse(error);
		std::vector<PortalSeam> seams;
		seams.reserve(topology.Mouths.size());
		for (const auto &mouth : topology.Mouths) {
			PortalSeam seam;
			seam.Centre = Vector(mouth.Centre);
			seam.Normal = Vector(mouth.Normal);
			seam.First = Vector(mouth.First);
			seam.Second = Vector(mouth.Second);
			seam.Up = Vector(mouth.Up);
			const auto &pose = mouth.Destination;
			seam.Destination =
				core::CFrame({pose[0], pose[1], pose[2]}, glm::quat(pose[6], pose[3], pose[4], pose[5]));
			seam.DestinationWorld = core::Name(mouth.DestinationWorld);
			seam.Scale = mouth.Scale;
			seam.Crosses = true;
			seam.Bidirectional = mouth.Bidirectional;
			seams.push_back(seam);
		}
		out = std::move(seams);
		error.clear();
		return true;
	}
}
