#include <engine/core/types/Vector2.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.vector2")

using Catch::Approx;
using engine::core::Vector2;

TEST_CASE("multiplication is component-wise, not a dot product", "[vector2]") {
	const Vector2 a{2.0f, 3.0f};
	const Vector2 b{5.0f, 6.0f};

	// Matching Roblox, and matching Vector3. Getting these confused is silent:
	// both compile and both return a number-shaped thing.
	REQUIRE((a * b) == Vector2{10.0f, 18.0f});
	REQUIRE(a.Dot(b) == Approx(28.0f));
}

TEST_CASE("the flat cross product is a signed area", "[vector2]") {
	// Positive turning from +X to +Y, which is the winding a caller asking this
	// question is trying to establish.
	REQUIRE(Vector2::XAxis.Cross(Vector2::YAxis) == Approx(1.0f));
	REQUIRE(Vector2::YAxis.Cross(Vector2::XAxis) == Approx(-1.0f));

	// Parallel vectors span no area, whatever their lengths.
	REQUIRE(Vector2{3.0f, 6.0f}.Cross(Vector2{1.0f, 2.0f}) == Approx(0.0f));
}

TEST_CASE("the unit of a zero vector is zero rather than NaN", "[vector2]") {
	REQUIRE(Vector2::Zero.Unit() == Vector2::Zero);
}

TEST_CASE("a unit vector has length one", "[vector2]") {
	const Vector2 unit = Vector2{3.0f, -4.0f}.Unit();

	REQUIRE(unit.Magnitude() == Approx(1.0f));
	REQUIRE(Vector2{3.0f, -4.0f}.Magnitude() == Approx(5.0f));
	REQUIRE(Vector2{3.0f, -4.0f}.MagnitudeSquared() == Approx(25.0f));
}

TEST_CASE("arithmetic behaves", "[vector2]") {
	const Vector2 a{1.0f, 2.0f};
	const Vector2 b{4.0f, 6.0f};

	REQUIRE((a + b) == Vector2{5.0f, 8.0f});
	REQUIRE((b - a) == Vector2{3.0f, 4.0f});
	REQUIRE((a * 2.0f) == Vector2{2.0f, 4.0f});
	REQUIRE((2.0f * a) == Vector2{2.0f, 4.0f});
	REQUIRE((b / 2.0f) == Vector2{2.0f, 3.0f});
	REQUIRE(-a == Vector2{-1.0f, -2.0f});
}

TEST_CASE("lerp hits both ends and extrapolates past them", "[vector2]") {
	const Vector2 from{0.0f, 0.0f};
	const Vector2 to{10.0f, 20.0f};

	REQUIRE(from.Lerp(to, 0.0f) == from);
	REQUIRE(from.Lerp(to, 1.0f) == to);
	REQUIRE(from.Lerp(to, 0.5f) == Vector2{5.0f, 10.0f});

	// Unclamped, deliberately: a caller passing 2 wanted to know they did.
	REQUIRE(from.Lerp(to, 2.0f) == Vector2{20.0f, 40.0f});
}

TEST_CASE("the named constants are what they say", "[vector2]") {
	REQUIRE(Vector2::Zero == Vector2{0.0f, 0.0f});
	REQUIRE(Vector2::One == Vector2{1.0f, 1.0f});
	REQUIRE(Vector2::XAxis == Vector2{1.0f, 0.0f});
	REQUIRE(Vector2::YAxis == Vector2{0.0f, 1.0f});
}

TEST_CASE("a default vector is the origin", "[vector2]") {
	REQUIRE(Vector2{} == Vector2::Zero);
}
