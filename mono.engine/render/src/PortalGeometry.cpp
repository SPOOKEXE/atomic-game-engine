#include "PortalPlayerIdentity.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/render/PortalGeometry.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <type_traits>

namespace engine::render {
	namespace {
		struct BorrowedGeometryRow {
			std::string_view Name;
			std::string_view Player;
			std::array<std::string_view, 9> Assets;
			PortalGeometryPose Pose{0, 0, 0, 0, 0, 0, 1};
			std::array<float, 3> HalfExtent{.5f, .5f, .5f};
			std::array<float, 3> Tint{1, 1, 1};
			std::array<float, 3> SurfaceColour{1, 1, 1};
			std::array<float, 3> EmissiveTint{1, 1, 1};
			float EmissiveStrength = 1;
			float Transparency = 0;
			float AlphaCutoff = .5f;
			std::array<float, 4> SeamPlane{};
			std::array<float, 3> SeamLight{};
			std::string_view Alpha = "opaque";
			std::string_view Resample = "default";
			bool CastShadow = true;
			uint32_t FirstJoint = 0;
			uint32_t JointCount = 0;
		};
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
		template <typename Row> bool ValidRow(const Row &row, size_t joints) {
			if (!Text(row.Name) || !ValidPortalPlayerIdentity(row.Player) || !ValidGeometryPose(row.Pose) ||
				!GeometryFinite(row.HalfExtent) ||
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
				(row.Resample != "default" && row.Resample != "pixelated") || row.FirstJoint > joints ||
				row.JointCount > joints - row.FirstJoint || (row.JointCount == 0 && row.FirstJoint != 0)) {
				return false;
			}
			const auto normal = std::span(row.SeamPlane).template first<3>();
			if (!GeometryUnit(normal) &&
				(normal[0] != 0 || normal[1] != 0 || normal[2] != 0 || row.SeamPlane[3] != 0)) {
				return false;
			}
			for (const auto &asset : row.Assets) {
				if (!Text(asset)) {
					return false;
				}
			}
			return true;
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
			for (const auto &row : geometry.Rows)
				if (!ValidRow(row, geometry.Joints.size())) return false;
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
		template <typename TextType>
		bool ReadText(core::ByteReader &reader, TextType &out, bool empty = true) {
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
	namespace {
		template <bool StoreRows>
		bool ParseGeometry(
			std::span<const std::byte> bytes, PortalGeometry *geometry, PortalGeometryMeasure &measure
		) {
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
			if (rows == 0 && joints != 0) return false;
			measure = {
				rows,
				joints,
				sizeof(PortalGeometry) + size_t(rows) * sizeof(PortalGeometryRow) +
					size_t(joints) * sizeof(PortalGeometryPose)
			};
			if constexpr (StoreRows) geometry->Rows.resize(rows);
			for (size_t index = 0; index < rows; ++index) {
				std::conditional_t<StoreRows, PortalGeometryRow, BorrowedGeometryRow> row;
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
				if (reader.Failed() || !ValidRow(row, joints)) return false;
				measure.MetadataBytes +=
					row.Name.size() + row.Player.size() + row.Alpha.size() + row.Resample.size();
				for (const auto &asset : row.Assets)
					measure.MetadataBytes += asset.size();
				if constexpr (StoreRows) geometry->Rows[index] = std::move(row);
			}
			if constexpr (StoreRows) geometry->Joints.resize(joints);
			for (size_t index = 0; index < joints; ++index) {
				PortalGeometryPose joint;
				GeometryFloats(reader, joint);
				if (!ValidGeometryPose(joint)) return false;
				if constexpr (StoreRows) geometry->Joints[index] = joint;
			}
			return !reader.Failed() && reader.AtEnd();
		}
	}
	bool
	MeasurePortalGeometry(std::span<const std::byte> bytes, PortalGeometryMeasure &out, std::string &error) {
		PortalGeometryMeasure measure;
		if (!ParseGeometry<false>(bytes, nullptr, measure)) {
			error = "invalid portal geometry";
			return false;
		}
		out = measure;
		error.clear();
		return true;
	}
	bool DecodePortalGeometry(std::span<const std::byte> bytes, PortalGeometry &out, std::string &error) {
		PortalGeometry geometry;
		PortalGeometryMeasure measure;
		if (!ParseGeometry<true>(bytes, &geometry, measure)) {
			error = "invalid portal geometry";
			return false;
		}
		out = std::move(geometry);
		error.clear();
		return true;
	}
}
