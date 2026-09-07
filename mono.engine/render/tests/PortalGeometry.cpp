#include <engine/render/PortalGeometry.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.portalgeometry")

namespace {
	using namespace engine::render;
	PortalGeometry Geometry() {
		PortalGeometry geometry;
		PortalGeometryRow row;
		row.Name = "Workspace.Player.Character.Head";
		row.Player = "91";
		for (size_t index = 0; index < row.Assets.size(); ++index) {
			row.Assets[index] = "asset/" + std::to_string(index);
		}
		row.Pose = {10, 20, 30, 0, 0, 1, 0};
		row.HalfExtent = {1, 2, 3};
		row.Tint = {.1f, .2f, .3f};
		row.SurfaceColour = {.4f, .5f, .6f};
		row.EmissiveTint = {.7f, .8f, .9f};
		row.EmissiveStrength = 4;
		row.Transparency = .25f;
		row.AlphaCutoff = .4f;
		row.SeamPlane = {0, 1, 0, 10};
		row.SeamLight = {1, 0, 0};
		row.Alpha = "transparency";
		row.Resample = "pixelated";
		row.CastShadow = false;
		row.JointCount = 2;
		geometry.Rows.push_back(row);
		geometry.Joints = {{0, 0, 0, 0, 0, 0, 1}, {1, 2, 3, 0, 1, 0, 0}};
		return geometry;
	}
}

TEST_CASE(
	"portal geometry owns material names clipped draws and skin palettes", "[render][portal-geometry]"
) {
	const auto geometry = Geometry();
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(EncodePortalGeometry(geometry, bytes, error));
	PortalGeometry decoded;
	REQUIRE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded == geometry);
	std::vector<std::byte> canonical;
	REQUIRE(EncodePortalGeometry(decoded, canonical, error));
	CHECK(canonical == bytes);
	for (size_t length = 0; length < bytes.size(); ++length) {
		CAPTURE(length);
		CHECK_FALSE(DecodePortalGeometry(std::span(bytes).first(length), decoded, error));
		CHECK(decoded == geometry);
	}
	bytes.push_back(std::byte{});
	CHECK_FALSE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded == geometry);
	REQUIRE(EncodePortalGeometry({}, bytes, error));
	REQUIRE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded.Rows.empty());
	CHECK(decoded.Joints.empty());
}

TEST_CASE(
	"portal geometry refuses invalid names poses planes and palette ranges", "[render][portal-geometry]"
) {
	auto geometry = Geometry();
	SECTION("noncanonical player") {
		geometry.Rows[0].Player = "01";
	}
	SECTION("overflow player") {
		geometry.Rows[0].Player = "9223372036854775808";
	}
	SECTION("overlong asset") {
		geometry.Rows[0].Assets[0].assign(257, 'x');
	}
	SECTION("embedded nul") {
		geometry.Rows[0].Assets[2] = std::string("a\0b", 3);
	}
	SECTION("unknown alpha") {
		geometry.Rows[0].Alpha = "unknown";
	}
	SECTION("unknown sampler") {
		geometry.Rows[0].Resample = "unknown";
	}
	SECTION("zero quaternion") {
		geometry.Rows[0].Pose = {};
	}
	SECTION("nonfinite colour") {
		geometry.Rows[0].Tint[0] = std::numeric_limits<float>::infinity();
	}
	SECTION("negative extent") {
		geometry.Rows[0].HalfExtent[1] = -1;
	}
	SECTION("nonunit cut") {
		geometry.Rows[0].SeamPlane[1] = 2;
	}
	SECTION("nonfinite joint") {
		geometry.Joints[0][1] = std::numeric_limits<float>::quiet_NaN();
	}
	SECTION("joint range overflow") {
		geometry.Rows[0].FirstJoint = UINT32_MAX;
	}
	SECTION("joint count overflow") {
		geometry.Rows[0].JointCount = UINT32_MAX;
	}
	SECTION("dangling unused palette") {
		geometry.Rows.clear();
	}
	std::vector<std::byte> bytes{std::byte{42}};
	std::string error;
	CHECK_FALSE(EncodePortalGeometry(geometry, bytes, error));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
	CHECK_FALSE(error.empty());
}

TEST_CASE("portal geometry bounds rows joints and decoded allocation", "[render][portal-geometry]") {
	PortalGeometry geometry;
	geometry.Joints.assign(MAX_PORTAL_GEOMETRY_JOINTS, {0, 0, 0, 0, 0, 0, 1});
	for (size_t index = 0; index < MAX_PORTAL_GEOMETRY_ROWS; ++index) {
		PortalGeometryRow row;
		row.Name = std::to_string(index);
		row.JointCount = static_cast<uint32_t>(geometry.Joints.size());
		geometry.Rows.push_back(std::move(row));
	}
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(EncodePortalGeometry(geometry, bytes, error));
	CHECK(bytes.size() <= MAX_PORTAL_GEOMETRY_BYTES);
	PortalGeometry decoded;
	REQUIRE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded == geometry);
	geometry.Joints.push_back({0, 0, 0, 0, 0, 0, 1});
	CHECK_FALSE(EncodePortalGeometry(geometry, bytes, error));
	geometry.Joints.pop_back();
	geometry.Rows.push_back({});
	CHECK_FALSE(EncodePortalGeometry(geometry, bytes, error));
	for (size_t index = 4; index < 8; ++index) {
		bytes[index] = std::byte{255};
	}
	CHECK_FALSE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded.Rows.size() == MAX_PORTAL_GEOMETRY_ROWS);
	bytes.resize(MAX_PORTAL_GEOMETRY_BYTES + 1);
	CHECK_FALSE(DecodePortalGeometry(bytes, decoded, error));
}

TEST_CASE("portal pictures preserve unnamed and identically named drawables", "[render][portal-geometry]") {
	PortalGeometry geometry;
	geometry.Rows.resize(2);
	geometry.Rows[1].Pose[0] = 5;
	std::vector<std::byte> bytes;
	std::string error;
	PortalGeometry decoded;
	REQUIRE(EncodePortalGeometry(geometry, bytes, error));
	REQUIRE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded == geometry);
	geometry.Rows[0].Name = geometry.Rows[1].Name = "Character.Part";
	REQUIRE(EncodePortalGeometry(geometry, bytes, error));
	REQUIRE(DecodePortalGeometry(bytes, decoded, error));
	CHECK(decoded == geometry);
}
