#include "PortalPlayerIdentity.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/render/PortalGeometry.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {
	namespace {
		constexpr uint32_t GEOMETRY_MAGIC = 0x32454750;
		bool Text(std::string_view text, bool empty = true) {
			return (empty || !text.empty()) && text.size() <= 256 &&
				   text.find('\0') == std::string_view::npos;
		}
		template <size_t N> bool GeometryFinite(const std::array<float, N> &values) {
			return std::all_of(values.begin(), values.end(), [](float value) {
				return std::isfinite(value);
			});
		}
		bool GeometryUnit(std::span<const float> values) {
			double norm = 0;
			for (float value : values) {
				norm += double(value) * value;
			}
			return std::abs(norm - 1) <= .001;
		}
		bool ValidGeometryPose(const PortalGeometryPose &pose) {
			return GeometryFinite(pose) && GeometryUnit(std::span(pose).last<4>());
		}
		bool Valid(const PortalGeometry &geometry) {
			if (geometry.Rows.size() > MAX_PORTAL_GEOMETRY_ROWS ||
				geometry.Joints.size() > MAX_PORTAL_GEOMETRY_JOINTS ||
				(geometry.Rows.empty() && !geometry.Joints.empty())) {
				return false;
			}
			for (const auto &pose : geometry.Joints) {
				if (!ValidGeometryPose(pose)) {
					return false;
				}
			}
			for (size_t index = 0; index < geometry.Rows.size(); ++index) {
				const auto &row = geometry.Rows[index];
				if (!Text(row.Name) || !ValidPortalPlayerIdentity(row.Player) ||
					!ValidGeometryPose(row.Pose) || !GeometryFinite(row.HalfExtent) ||
					!std::all_of(
						row.HalfExtent.begin(), row.HalfExtent.end(), [](float value) { return value > 0; }
					) ||
					!GeometryFinite(row.Tint) || !GeometryFinite(row.SurfaceColour) ||
					!GeometryFinite(row.EmissiveTint) || !std::isfinite(row.EmissiveStrength) ||
					row.EmissiveStrength < 0 || !std::isfinite(row.Transparency) || row.Transparency < 0 ||
					row.Transparency > 1 || !std::isfinite(row.AlphaCutoff) || row.AlphaCutoff < 0 ||
					row.AlphaCutoff > 1 || !GeometryFinite(row.SeamPlane) || !GeometryFinite(row.SeamLight) ||
					(row.Alpha != "opaque" && row.Alpha != "overlay" && row.Alpha != "transparency" &&
					 row.Alpha != "tint-mask") ||
					(row.Resample != "default" && row.Resample != "pixelated") ||
					row.FirstJoint > geometry.Joints.size() ||
					row.JointCount > geometry.Joints.size() - row.FirstJoint ||
					(row.JointCount == 0 && row.FirstJoint != 0)) {
					return false;
				}
				const auto normal = std::span(row.SeamPlane).first<3>();
				if (!GeometryUnit(normal) &&
					(normal[0] != 0 || normal[1] != 0 || normal[2] != 0 || row.SeamPlane[3] != 0)) {
					return false;
				}
				for (const auto &asset : row.Assets) {
					if (!Text(asset)) {
						return false;
					}
				}
			}
			return true;
		}
		template <size_t N>
		void GeometryFloats(core::ByteWriter &writer, const std::array<float, N> &values) {
			for (float value : values) {
				writer.WriteFloat(value == 0 ? 0.0f : value);
			}
		}
		template <size_t N> void GeometryFloats(core::ByteReader &reader, std::array<float, N> &values) {
			for (float &value : values) {
				value = reader.ReadFloat();
			}
		}
		bool ReadText(core::ByteReader &reader, std::string &out, bool empty = true) {
			const auto value = reader.ReadString();
			if (reader.Failed() || !Text(value, empty)) {
				return false;
			}
			out = value;
			return true;
		}
	}
	bool
	EncodePortalGeometry(const PortalGeometry &geometry, std::vector<std::byte> &out, std::string &error) {
		if (!Valid(geometry)) {
			error = "invalid portal geometry";
			return false;
		}
		core::ByteWriter writer;
		writer.WriteUInt32(GEOMETRY_MAGIC);
		writer.WriteUInt32(static_cast<uint32_t>(geometry.Rows.size()));
		writer.WriteUInt32(static_cast<uint32_t>(geometry.Joints.size()));
		for (const auto &row : geometry.Rows) {
			writer.WriteString(row.Name);
			writer.WriteString(row.Player);
			for (const auto &asset : row.Assets) {
				writer.WriteString(asset);
			}
			GeometryFloats(writer, row.Pose);
			GeometryFloats(writer, row.HalfExtent);
			GeometryFloats(writer, row.Tint);
			GeometryFloats(writer, row.SurfaceColour);
			GeometryFloats(writer, row.EmissiveTint);
			writer.WriteFloat(row.EmissiveStrength);
			writer.WriteFloat(row.Transparency);
			writer.WriteFloat(row.AlphaCutoff);
			GeometryFloats(writer, row.SeamPlane);
			GeometryFloats(writer, row.SeamLight);
			writer.WriteString(row.Alpha);
			writer.WriteString(row.Resample);
			writer.WriteUInt8(row.CastShadow ? 1 : 0);
			writer.WriteUInt32(row.FirstJoint);
			writer.WriteUInt32(row.JointCount);
		}
		for (const auto &joint : geometry.Joints) {
			GeometryFloats(writer, joint);
		}
		if (writer.Size() > MAX_PORTAL_GEOMETRY_BYTES) {
			error = "portal geometry exceeds byte budget";
			return false;
		}
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		error.clear();
		return true;
	}
	bool DecodePortalGeometry(std::span<const std::byte> bytes, PortalGeometry &out, std::string &error) {
		error = "invalid portal geometry";
		if (bytes.size() > MAX_PORTAL_GEOMETRY_BYTES) {
			return false;
		}
		core::ByteReader reader(bytes);
		if (reader.ReadUInt32() != GEOMETRY_MAGIC) {
			return false;
		}
		const uint32_t rows = reader.ReadUInt32();
		const uint32_t joints = reader.ReadUInt32();
		if (reader.Failed() || rows > MAX_PORTAL_GEOMETRY_ROWS || joints > MAX_PORTAL_GEOMETRY_JOINTS ||
			uint64_t(joints) * 28 + uint64_t(rows) * 4 > reader.Remaining()) {
			return false;
		}
		PortalGeometry geometry;
		geometry.Rows.resize(rows);
		for (auto &row : geometry.Rows) {
			if (!ReadText(reader, row.Name)) return false;
			const auto player = reader.ReadString();
			if (reader.Failed() || !ValidPortalPlayerIdentity(player)) return false;
			row.Player = player;
			for (auto &asset : row.Assets) {
				if (!ReadText(reader, asset)) {
					return false;
				}
			}
			GeometryFloats(reader, row.Pose);
			GeometryFloats(reader, row.HalfExtent);
			GeometryFloats(reader, row.Tint);
			GeometryFloats(reader, row.SurfaceColour);
			GeometryFloats(reader, row.EmissiveTint);
			row.EmissiveStrength = reader.ReadFloat();
			row.Transparency = reader.ReadFloat();
			row.AlphaCutoff = reader.ReadFloat();
			GeometryFloats(reader, row.SeamPlane);
			GeometryFloats(reader, row.SeamLight);
			if (!ReadText(reader, row.Alpha, false) || !ReadText(reader, row.Resample, false)) {
				return false;
			}
			const auto shadow = reader.ReadUInt8();
			if (shadow > 1) {
				return false;
			}
			row.CastShadow = shadow != 0;
			row.FirstJoint = reader.ReadUInt32();
			row.JointCount = reader.ReadUInt32();
		}
		geometry.Joints.resize(joints);
		for (auto &joint : geometry.Joints) {
			GeometryFloats(reader, joint);
		}
		if (reader.Failed() || !reader.AtEnd() || !Valid(geometry)) {
			return false;
		}
		out = std::move(geometry);
		error.clear();
		return true;
	}
}
