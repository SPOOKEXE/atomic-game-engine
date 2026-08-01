#include <engine/core/types/Vector3.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.vector3")

using Catch::Approx;
using engine::core::Vector3;

TEST_CASE("multiplication is component-wise, not a dot product", "[vector3]") {
	const Vector3 a { 2.0f, 3.0f, 4.0f };
	const Vector3 b { 5.0f, 6.0f, 7.0f };

	// Matching Roblox. The dot product is spelled Dot for exactly this reason,
	// and getting them confused is silent — both compile and both return.
	REQUIRE((a * b) == Vector3 { 10.0f, 18.0f, 28.0f });
	REQUIRE(a.Dot(b) == Approx(56.0f));
}

TEST_CASE("cross follows the right hand", "[vector3]") {
	REQUIRE(Vector3::XAxis.Cross(Vector3::YAxis) == Vector3::ZAxis);
	REQUIRE(Vector3::YAxis.Cross(Vector3::ZAxis) == Vector3::XAxis);
	REQUIRE(Vector3::ZAxis.Cross(Vector3::XAxis) == Vector3::YAxis);

	// Anti-commutative. A sign error here inverts every normal.
	REQUIRE(Vector3::YAxis.Cross(Vector3::XAxis) == -Vector3::ZAxis);
}

TEST_CASE("the unit of a zero vector is zero rather than NaN", "[vector3]") {
	// A zero vector has no direction, and saying so beats a NaN that surfaces
	// three subsystems away where nothing looks like the cause.
	REQUIRE(Vector3::Zero.Unit() == Vector3::Zero);
}

TEST_CASE("a unit vector has length one", "[vector3]") {
	const Vector3 unit = Vector3 { 3.0f, -4.0f, 12.0f }.Unit();

	REQUIRE(unit.Magnitude() == Approx(1.0f));
	REQUIRE(Vector3 { 3.0f, -4.0f, 12.0f }.Magnitude() == Approx(13.0f));
	// Squared, for comparisons that do not need the square root.
	REQUIRE(Vector3 { 3.0f, -4.0f, 12.0f }.MagnitudeSquared() == Approx(169.0f));
}

TEST_CASE("arithmetic behaves", "[vector3]") {
	const Vector3 a { 1.0f, 2.0f, 3.0f };
	const Vector3 b { 4.0f, 5.0f, 6.0f };

	REQUIRE((a + b) == Vector3 { 5.0f, 7.0f, 9.0f });
	REQUIRE((b - a) == Vector3 { 3.0f, 3.0f, 3.0f });
	REQUIRE((a * 2.0f) == Vector3 { 2.0f, 4.0f, 6.0f });
	REQUIRE((2.0f * a) == Vector3 { 2.0f, 4.0f, 6.0f });
	REQUIRE((b / 2.0f) == Vector3 { 2.0f, 2.5f, 3.0f });
	REQUIRE(-a == Vector3 { -1.0f, -2.0f, -3.0f });
}

TEST_CASE("lerp hits both ends and the middle", "[vector3]") {
	const Vector3 from { 0.0f, 0.0f, 0.0f };
	const Vector3 to { 10.0f, 20.0f, 30.0f };

	REQUIRE(from.Lerp(to, 0.0f) == from);
	REQUIRE(from.Lerp(to, 1.0f) == to);
	REQUIRE(from.Lerp(to, 0.5f) == Vector3 { 5.0f, 10.0f, 15.0f });
}

TEST_CASE("the named constants are what they say", "[vector3]") {
	REQUIRE(Vector3::Zero == Vector3 { 0.0f, 0.0f, 0.0f });
	REQUIRE(Vector3::One == Vector3 { 1.0f, 1.0f, 1.0f });
	REQUIRE(Vector3::XAxis == Vector3 { 1.0f, 0.0f, 0.0f });
	REQUIRE(Vector3::YAxis == Vector3 { 0.0f, 1.0f, 0.0f });
	REQUIRE(Vector3::ZAxis == Vector3 { 0.0f, 0.0f, 1.0f });
}
