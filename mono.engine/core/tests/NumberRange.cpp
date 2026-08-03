#include <engine/core/types/NumberRange.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.numberrange")

using Catch::Approx;
using engine::core::NumberRange;

TEST_CASE("the one-argument form is the degenerate range", "[numberrange]") {
	const NumberRange point{5.0f};

	REQUIRE(point.Minimum == Approx(5.0f));
	REQUIRE(point.Maximum == Approx(5.0f));
	REQUIRE(point.Span() == Approx(0.0f));
	REQUIRE(point.Contains(5.0f));
	REQUIRE_FALSE(point.Contains(5.001f));
}

TEST_CASE("containment includes both ends", "[numberrange]") {
	const NumberRange range{-2.0f, 8.0f};

	REQUIRE(range.Contains(-2.0f));
	REQUIRE(range.Contains(8.0f));
	REQUIRE(range.Contains(0.0f));
	REQUIRE_FALSE(range.Contains(-2.001f));
	REQUIRE_FALSE(range.Contains(8.001f));
}

TEST_CASE("lerp runs from the low end to the high one and does not clamp", "[numberrange]") {
	const NumberRange range{10.0f, 20.0f};

	REQUIRE(range.Lerp(0.0f) == Approx(10.0f));
	REQUIRE(range.Lerp(1.0f) == Approx(20.0f));
	REQUIRE(range.Lerp(0.5f) == Approx(15.0f));

	// Unclamped, matching every other Lerp in these headers.
	REQUIRE(range.Lerp(2.0f) == Approx(30.0f));
	REQUIRE(range.Lerp(-1.0f) == Approx(0.0f));
}

TEST_CASE("clamp does clamp", "[numberrange]") {
	const NumberRange range{10.0f, 20.0f};

	REQUIRE(range.Clamp(5.0f) == Approx(10.0f));
	REQUIRE(range.Clamp(25.0f) == Approx(20.0f));
	REQUIRE(range.Clamp(15.0f) == Approx(15.0f));
}

TEST_CASE("nothing reorders a range built backwards", "[numberrange]") {
	// Same choice `AABB` and `Rect` make: a backwards range contains nothing,
	// which surfaces the caller's mistake at the first test.
	const NumberRange backwards{10.0f, 2.0f};

	REQUIRE_FALSE(backwards.Contains(5.0f));
	REQUIRE_FALSE(backwards.Contains(10.0f));
	REQUIRE(backwards.Span() == Approx(-8.0f));
}

TEST_CASE("a default range holds only zero", "[numberrange]") {
	REQUIRE(NumberRange{} == NumberRange{0.0f});
	REQUIRE(NumberRange{}.Contains(0.0f));
}
