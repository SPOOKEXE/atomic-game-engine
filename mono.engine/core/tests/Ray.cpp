#include <engine/core/types/Ray.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.ray")
// A ray is two Vector3s and PointAt is Vector3 arithmetic, so a change to
// Vector3 has to re-run this too.
TEST_DEPENDS("engine.core.types.vector3")

using Catch::Approx;
using engine::core::Ray;
using engine::core::RayHit;
using engine::core::Vector3;

TEST_CASE("a default ray has no direction and so hits nothing", "[ray]") {
	const Ray degenerate;

	// The same answer Vector3::Unit gives a zero vector: saying "there is no
	// direction" rather than handing a query a NaN to propagate. Every query
	// refuses a ray shaped like this.
	REQUIRE(degenerate.Origin == Vector3::Zero);
	REQUIRE(degenerate.Direction == Vector3::Zero);
}

TEST_CASE("PointAt walks along the direction in the direction's own units", "[ray]") {
	const Ray ray{Vector3{1.0f, 2.0f, 3.0f}, Vector3::XAxis};

	REQUIRE(ray.PointAt(0.0f) == ray.Origin);
	REQUIRE(ray.PointAt(4.0f) == Vector3{5.0f, 2.0f, 3.0f});

	// Behind the origin is a real answer. A query decides whether it wants one;
	// the arithmetic does not clamp on the query's behalf.
	REQUIRE(ray.PointAt(-2.0f) == Vector3{-1.0f, 2.0f, 3.0f});
}

TEST_CASE("a distance along a unit ray is a distance in world units", "[ray]") {
	// The whole reason the direction must be unit length. Given a diagonal that
	// was normalised, ten units along the ray is ten units away from the
	// origin — which is what every reported hit distance means.
	const Ray ray{Vector3::Zero, Vector3{3.0f, -4.0f, 12.0f}.Unit()};

	REQUIRE(ray.PointAt(10.0f).Magnitude() == Approx(10.0f));
}

TEST_CASE("a non-unit direction scales every distance, which is why nothing normalises", "[ray]") {
	// Deliberately constructed wrong, to pin the behaviour the type documents
	// rather than to endorse it. Ten "units" along a direction of length two is
	// twenty units of world space, and no query can tell that happened.
	const Ray wrong{Vector3::Zero, Vector3{2.0f, 0.0f, 0.0f}};

	REQUIRE(wrong.PointAt(10.0f).Magnitude() == Approx(20.0f));
}

TEST_CASE("two rays are equal only when both origin and direction match", "[ray]") {
	const Ray ray{Vector3{1.0f, 0.0f, 0.0f}, Vector3::YAxis};

	REQUIRE(ray == Ray{Vector3{1.0f, 0.0f, 0.0f}, Vector3::YAxis});
	REQUIRE_FALSE(ray == Ray{Vector3::Zero, Vector3::YAxis});
	REQUIRE_FALSE(ray == Ray{Vector3{1.0f, 0.0f, 0.0f}, Vector3::XAxis});
}

TEST_CASE("a default hit names nothing and is not a hit that says so", "[ray]") {
	// There is no validity flag on purpose: a query that found nothing returns
	// no hit at all. This case exists to keep that true — a `bool Valid` added
	// here would make reading the position out of a miss compile.
	const RayHit hit;

	REQUIRE(hit.Id == 0u);
	REQUIRE(hit.Distance == 0.0f);
	REQUIRE(hit.Position == Vector3::Zero);
	REQUIRE(hit.Normal == Vector3::Zero);
}
