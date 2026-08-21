#include <engine/core/types/Vector3.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

TEST_SUITE_ID("engine.core.types.vector3")

using Catch::Approx;
using engine::core::Vector3;

TEST_CASE("multiplication is component-wise, not a dot product", "[vector3]") {
	const Vector3 a{2.0f, 3.0f, 4.0f};
	const Vector3 b{5.0f, 6.0f, 7.0f};

	// Matching Roblox. The dot product is spelled Dot for exactly this reason,
	// and getting them confused is silent - both compile and both return.
	REQUIRE((a * b) == Vector3{10.0f, 18.0f, 28.0f});
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
	const Vector3 unit = Vector3{3.0f, -4.0f, 12.0f}.Unit();

	REQUIRE(unit.Magnitude() == Approx(1.0f));
	REQUIRE(Vector3{3.0f, -4.0f, 12.0f}.Magnitude() == Approx(13.0f));
	// Squared, for comparisons that do not need the square root.
	REQUIRE(Vector3{3.0f, -4.0f, 12.0f}.MagnitudeSquared() == Approx(169.0f));
}

TEST_CASE("arithmetic behaves", "[vector3]") {
	const Vector3 a{1.0f, 2.0f, 3.0f};
	const Vector3 b{4.0f, 5.0f, 6.0f};

	REQUIRE((a + b) == Vector3{5.0f, 7.0f, 9.0f});
	REQUIRE((b - a) == Vector3{3.0f, 3.0f, 3.0f});
	REQUIRE((a * 2.0f) == Vector3{2.0f, 4.0f, 6.0f});
	REQUIRE((2.0f * a) == Vector3{2.0f, 4.0f, 6.0f});
	REQUIRE((b / 2.0f) == Vector3{2.0f, 2.5f, 3.0f});
	REQUIRE(-a == Vector3{-1.0f, -2.0f, -3.0f});
}

TEST_CASE("lerp hits both ends and the middle", "[vector3]") {
	const Vector3 from{0.0f, 0.0f, 0.0f};
	const Vector3 to{10.0f, 20.0f, 30.0f};

	REQUIRE(from.Lerp(to, 0.0f) == from);
	REQUIRE(from.Lerp(to, 1.0f) == to);
	REQUIRE(from.Lerp(to, 0.5f) == Vector3{5.0f, 10.0f, 15.0f});
}

TEST_CASE("division is component-wise, like multiplication", "[vector3]") {
	const Vector3 a{10.0f, 9.0f, 8.0f};

	REQUIRE((a / Vector3{2.0f, 3.0f, 4.0f}) == Vector3{5.0f, 3.0f, 2.0f});
	REQUIRE((a / 2.0f) == Vector3{5.0f, 4.5f, 4.0f});
}

TEST_CASE("the component-wise unaries round the way Roblox's do", "[vector3]") {
	const Vector3 a{-2.6f, 5.1f, 8.8f};

	REQUIRE(a.Abs() == Vector3{2.6f, 5.1f, 8.8f});
	REQUIRE(a.Ceil() == Vector3{-2.0f, 6.0f, 9.0f});

	// **Down rather than toward zero**, which is the entry a hand-written
	// version gets wrong: `-2.6` floors to `-3`, and truncating instead puts a
	// one-unit step across the origin. `//` is built on this.
	REQUIRE(a.Floor() == Vector3{-3.0f, 5.0f, 8.0f});

	// A zero component has no sign and answers zero, so a vector already flat
	// against an axis plane stays flat.
	REQUIRE(Vector3{-2.6f, 5.1f, 0.0f}.Sign() == Vector3{-1.0f, 1.0f, 0.0f});
}

TEST_CASE("max and min compare each component separately", "[vector3]") {
	// Not "the longer vector": every component is chosen on its own, so the
	// result is usually neither operand. This is what an axis-aligned box is
	// grown by.
	const Vector3 a{1.0f, 2.0f, 1.0f};
	const Vector3 b{2.0f, 1.0f, 2.0f};

	REQUIRE(a.Max(b) == Vector3{2.0f, 2.0f, 2.0f});
	REQUIRE(a.Min(b) == Vector3{1.0f, 1.0f, 1.0f});
}

TEST_CASE("angle is unsigned until an axis says which way", "[vector3]") {
	const Vector3 right = Vector3::XAxis;
	const Vector3 forward = Vector3::ZAxis;
	const auto quarter = static_cast<float>(std::numbers::pi / 2.0);

	REQUIRE(right.Angle(forward) == Approx(quarter));
	REQUIRE(forward.Angle(right) == Approx(quarter));

	// **The sign is the whole reason the axis argument exists**: without it a
	// steering routine knows how far off it is and not which way to turn. X to
	// Z crosses to -Y, so +Y sees it as negative and -Y as positive.
	REQUIRE(right.Angle(forward, Vector3::YAxis) == Approx(-quarter));
	REQUIRE(right.Angle(forward, -Vector3::YAxis) == Approx(quarter));

	// Opposite directions are pi rather than a NaN out of a dot product that
	// rounded past -1.
	REQUIRE(right.Angle(-right) == Approx(static_cast<float>(std::numbers::pi)));
	REQUIRE(right.Angle(right) == Approx(0.0f));
}

TEST_CASE("fuzzy equality scales its tolerance with the vectors", "[vector3]") {
	// One epsilon has to work for a normal and for a point a thousand studs
	// out. An absolute tolerance is either useless at that distance or far too
	// loose at the origin, so the tolerance grows with the longer operand.
	const Vector3 near{1.0f, 0.0f, 0.0f};

	REQUIRE(near.FuzzyEq(Vector3{1.0f + 1.0e-6f, 0.0f, 0.0f}));
	REQUIRE_FALSE(near.FuzzyEq(Vector3{1.0f + 1.0e-3f, 0.0f, 0.0f}));

	const Vector3 far{1000.0f, 0.0f, 0.0f};
	REQUIRE(far.FuzzyEq(Vector3{1000.0f + 1.0e-3f, 0.0f, 0.0f}));

	// An explicit epsilon is absolute at unit scale, so a caller who wants a
	// coarse comparison gets one.
	REQUIRE(near.FuzzyEq(Vector3{1.5f, 0.0f, 0.0f}, 1.0f));
	REQUIRE(Vector3::Zero.FuzzyEq(Vector3::Zero));
}

TEST_CASE("the named constants are what they say", "[vector3]") {
	REQUIRE(Vector3::Zero == Vector3{0.0f, 0.0f, 0.0f});
	REQUIRE(Vector3::One == Vector3{1.0f, 1.0f, 1.0f});
	REQUIRE(Vector3::XAxis == Vector3{1.0f, 0.0f, 0.0f});
	REQUIRE(Vector3::YAxis == Vector3{0.0f, 1.0f, 0.0f});
	REQUIRE(Vector3::ZAxis == Vector3{0.0f, 0.0f, 1.0f});
}
