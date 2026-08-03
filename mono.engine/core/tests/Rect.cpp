#include <engine/core/types/Rect.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.rect")

using Catch::Approx;
using engine::core::Rect;
using engine::core::Vector2;

TEST_CASE("size, width, height and centre come off the corners", "[rect]") {
	const Rect box{10.0f, 20.0f, 40.0f, 100.0f};

	REQUIRE(box.Width() == Approx(30.0f));
	REQUIRE(box.Height() == Approx(80.0f));
	REQUIRE(box.Size() == Vector2{30.0f, 80.0f});
	REQUIRE(box.Center() == Vector2{25.0f, 60.0f});
}

TEST_CASE("containment includes the edges", "[rect]") {
	const Rect box{0.0f, 0.0f, 10.0f, 10.0f};

	REQUIRE(box.Contains(Vector2{5.0f, 5.0f}));
	REQUIRE(box.Contains(Vector2{0.0f, 0.0f}));
	REQUIRE(box.Contains(Vector2{10.0f, 10.0f}));
	REQUIRE_FALSE(box.Contains(Vector2{10.001f, 5.0f}));
	REQUIRE_FALSE(box.Contains(Vector2{-0.001f, 5.0f}));
}

TEST_CASE("touching rectangles intersect", "[rect]") {
	const Rect left{0.0f, 0.0f, 10.0f, 10.0f};
	const Rect touching{10.0f, 0.0f, 20.0f, 10.0f};
	const Rect apart{10.001f, 0.0f, 20.0f, 10.0f};

	// Inclusive, for the reason `AABB::Overlaps` is: an exclusive test separates
	// things resting exactly against each other for one frame at a time.
	REQUIRE(left.Intersects(touching));
	REQUIRE_FALSE(left.Intersects(apart));
}

TEST_CASE("an intersection that shares nothing comes back empty", "[rect]") {
	const Rect left{0.0f, 0.0f, 10.0f, 10.0f};
	const Rect right{20.0f, 20.0f, 30.0f, 30.0f};

	const Rect overlap = left.Intersection(right);
	REQUIRE(overlap.Empty());

	// And an overlap that does exist is the tighter of each bound.
	const Rect inner = left.Intersection(Rect{5.0f, -5.0f, 20.0f, 8.0f});
	REQUIRE_FALSE(inner.Empty());
	REQUIRE(inner == Rect{5.0f, 0.0f, 10.0f, 8.0f});
}

TEST_CASE("a zero-area rectangle is not empty", "[rect]") {
	// A point and a line are degenerate, not empty. A caller clipping against
	// one wants the distinction: an empty result means "nothing here", and a
	// zero-area one means "exactly this edge".
	REQUIRE_FALSE(Rect{5.0f, 5.0f, 5.0f, 5.0f}.Empty());
	REQUIRE_FALSE(Rect{0.0f, 5.0f, 10.0f, 5.0f}.Empty());

	REQUIRE(Rect{5.0f, 0.0f, 4.0f, 10.0f}.Empty());
	REQUIRE(Rect{0.0f, 5.0f, 10.0f, 4.0f}.Empty());
}

TEST_CASE("nothing reorders a rectangle built backwards", "[rect]") {
	// Swapping the corners in a constructor would hide the caller's bug. The
	// negative size is the signal that something built it the wrong way round.
	const Rect backwards{10.0f, 10.0f, 0.0f, 0.0f};

	REQUIRE(backwards.Empty());
	REQUIRE(backwards.Width() == Approx(-10.0f));
	REQUIRE_FALSE(backwards.Contains(Vector2{5.0f, 5.0f}));
}

TEST_CASE("a default rectangle is the degenerate one at the origin", "[rect]") {
	REQUIRE(Rect{} == Rect{Vector2::Zero, Vector2::Zero});
	REQUIRE_FALSE(Rect{}.Empty());
}
