#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::render {
	inline constexpr size_t MAX_PORTAL_GEOMETRY_ROWS = 256;
	inline constexpr size_t MAX_PORTAL_GEOMETRY_JOINTS = 4096;
	inline constexpr size_t MAX_PORTAL_GEOMETRY_BYTES = 1024 * 1024;

	// Position XYZ followed by unit quaternion XYZW. Already mapped to the
	// destination frame; palette transforms retain the renderer's local convention.
	using PortalGeometryPose = std::array<float, 7>;
	struct PortalGeometryRow {
		// Optional diagnostic path. Draw rows do not transfer ECS identity, so
		// unnamed parts and repeated names remain distinct entries in this picture.
		std::string Name;
		// Canonical account identity, empty when the row has no known player owner.
		std::string Player;
		// Mesh, colour, normal, roughness, occlusion, height, metalness, emission, shader.
		std::array<std::string, 9> Assets;
		PortalGeometryPose Pose{0, 0, 0, 0, 0, 0, 1};
		std::array<float, 3> HalfExtent{.5f, .5f, .5f};
		std::array<float, 3> Tint{1, 1, 1};
		std::array<float, 3> SurfaceColour{1, 1, 1};
		std::array<float, 3> EmissiveTint{1, 1, 1};
		float EmissiveStrength = 1;
		float Transparency = 0;
		float AlphaCutoff = .5f;
		// Keep dot(XYZ, position) >= W; zero XYZ disables the cut.
		std::array<float, 4> SeamPlane{};
		std::array<float, 3> SeamLight{};
		std::string Alpha = "opaque";
		std::string Resample = "default";
		bool CastShadow = true;
		uint32_t FirstJoint = 0;
		uint32_t JointCount = 0;
		bool operator==(const PortalGeometryRow &) const = default;
	};
	struct PortalGeometry {
		std::vector<PortalGeometryRow> Rows;
		std::vector<PortalGeometryPose> Joints;
		bool operator==(const PortalGeometry &) const = default;
	};

	struct PortalGeometryMeasure {
		size_t Rows = 0;
		size_t Joints = 0;
		// Decoded object, rows, palette and text payload; excludes encoded input bytes.
		size_t MetadataBytes = 0;
		bool operator==(const PortalGeometryMeasure &) const = default;
	};
	// Validate borrowed bytes without allocating geometry or text. Output is transactional.
	bool
	MeasurePortalGeometry(std::span<const std::byte> bytes, PortalGeometryMeasure &out, std::string &error);

	// Canonical owned data only. No interning, ECS identity or device token enters
	// this codec. Counts, text, poses and palette ranges are checked transactionally.
	bool
	EncodePortalGeometry(const PortalGeometry &geometry, std::vector<std::byte> &out, std::string &error);
	bool DecodePortalGeometry(std::span<const std::byte> bytes, PortalGeometry &out, std::string &error);
}
