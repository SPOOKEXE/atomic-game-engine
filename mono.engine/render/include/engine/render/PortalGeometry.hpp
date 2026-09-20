#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::render {
	// Maximum drawable rows accepted from one portal geometry payload.
	inline constexpr size_t MAX_PORTAL_GEOMETRY_ROWS = 256;
	// Maximum palette poses accepted from one portal geometry payload.
	inline constexpr size_t MAX_PORTAL_GEOMETRY_JOINTS = 4096;
	// Maximum encoded portal geometry payload in bytes.
	inline constexpr size_t MAX_PORTAL_GEOMETRY_BYTES = 1024 * 1024;

	// Position XYZ followed by unit quaternion XYZW. Already mapped to the
	// destination frame; palette transforms retain the renderer's local convention.
	// Position XYZ and destination-frame unit quaternion XYZW.
	using PortalGeometryPose = std::array<float, 7>;
	// One mesh draw and its material and skeletal-palette references.
	struct PortalGeometryRow {
		// Optional diagnostic path. Draw rows do not transfer ECS identity, so
		// unnamed parts and repeated names remain distinct entries in this picture.
		std::string Name;
		// Canonical account identity, empty when the row has no known player owner.
		std::string Player;
		// Mesh, colour, normal, roughness, occlusion, height, metalness, emission, shader.
		// Content names in mesh, colour, normal, roughness, occlusion, height, metalness, emission, shader
		// order.
		std::array<std::string, 9> Assets;
		// Row pose expressed in the destination frame.
		PortalGeometryPose Pose{0, 0, 0, 0, 0, 0, 1};
		// Axis-aligned half-size in destination-frame metres.
		std::array<float, 3> HalfExtent{.5f, .5f, .5f};
		// Linear RGB albedo tint.
		std::array<float, 3> Tint{1, 1, 1};
		// Linear RGB surface colour override.
		std::array<float, 3> SurfaceColour{1, 1, 1};
		// Linear RGB emission tint.
		std::array<float, 3> EmissiveTint{1, 1, 1};
		// Scalar multiplier applied to EmissiveTint.
		float EmissiveStrength = 1;
		// Material transparency from opaque zero to fully transparent one.
		float Transparency = 0;
		// Alpha-test threshold used by cutout materials.
		float AlphaCutoff = .5f;
		// Keep dot(XYZ, position) >= W; zero XYZ disables the cut.
		std::array<float, 4> SeamPlane{};
		// Directional seam-light contribution in destination-frame coordinates.
		std::array<float, 3> SeamLight{};
		// Alpha blend mode selected by the source material.
		std::string Alpha = "opaque";
		// Texture resampling mode selected by the source material.
		std::string Resample = "default";
		// Whether this row contributes to portal shadow maps.
		bool CastShadow = true;
		// First pose in PortalGeometry::Joints used by this row.
		uint32_t FirstJoint = 0;
		// Number of consecutive palette poses used by this row.
		uint32_t JointCount = 0;
		// Value comparison for geometry cache and wire tests.
		bool operator==(const PortalGeometryRow &) const = default;
	};
	// Owned geometry decoded from one authenticated portal payload.
	struct PortalGeometry {
		// Draw rows in sender declaration order.
		std::vector<PortalGeometryRow> Rows;
		// Shared pose palette addressed by each row's joint range.
		std::vector<PortalGeometryPose> Joints;
		// Value comparison for cache reuse and wire tests.
		bool operator==(const PortalGeometry &) const = default;
	};

	// Allocation-free summary of a validated encoded geometry payload.
	struct PortalGeometryMeasure {
		// Number of decoded draw rows.
		size_t Rows = 0;
		// Number of decoded palette poses.
		size_t Joints = 0;
		// Decoded object, rows, palette and text payload; excludes encoded input bytes.
		size_t MetadataBytes = 0;
		// Value comparison for measured payload budgets.
		bool operator==(const PortalGeometryMeasure &) const = default;
	};
	// Validate borrowed bytes without allocating geometry or text. Output is transactional.
	bool
	MeasurePortalGeometry(std::span<const std::byte> bytes, PortalGeometryMeasure &out, std::string &error);

	// Canonical owned data only. No interning, ECS identity or device token enters
	// this codec. Counts, text, poses and palette ranges are checked transactionally.
	bool
	EncodePortalGeometry(const PortalGeometry &geometry, std::vector<std::byte> &out, std::string &error);
	// Decodes canonical owned geometry after validating all payload bounds and palette ranges.
	bool DecodePortalGeometry(std::span<const std::byte> bytes, PortalGeometry &out, std::string &error);
}
