// Shot encoding and hit-test regression cases.

#include <engine/examples/Shooting.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.examples.shooting")

using Catch::Approx;
using engine::core::Ray;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::examples::DecodeShot;
using engine::examples::EncodeShot;
using engine::examples::Hit;
using engine::examples::MAXIMUM_SHOT_RANGE;
using engine::examples::NearestHit;
using engine::examples::Shot;
using engine::examples::Target;

namespace {
	Entity At(uint32_t id) {
		Entity entity;
		entity.Id = id;
		return entity;
	}

	// A shot down positive X from the origin.
	Shot Along(float range = 100.0f) {
		Shot shot;
		shot.Aim = Ray(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f});
		shot.Range = range;
		return shot;
	}

	Target Sphere(uint32_t id, float x, float radius = 0.5f) {
		Target target;
		target.Entity = At(id);
		target.At = Vector3{x, 0.0f, 0.0f};
		target.Radius = radius;
		return target;
	}
}

TEST_CASE("a shot round-trips", "[examples][shooting]") {
	Shot sent;
	sent.Aim = Ray(Vector3{1.0f, 2.0f, 3.0f}, Vector3{0.0f, 0.0f, -1.0f});
	sent.Range = 42.0f;

	Shot read;
	REQUIRE(DecodeShot(EncodeShot(sent), read));

	CHECK(read.Aim.Origin.X == Approx(1.0f));
	CHECK(read.Aim.Origin.Z == Approx(3.0f));
	CHECK(read.Aim.Direction.Z == Approx(-1.0f));
	CHECK(read.Range == Approx(42.0f));
}

TEST_CASE("a direction that is not unit length is refused", "[examples][shooting]") {
	Shot read;

	SECTION("zero") {
		// Zero direction must not be normalized or accepted.
		Shot shot = Along();
		shot.Aim.Direction = Vector3{0.0f, 0.0f, 0.0f};
		CHECK_FALSE(DecodeShot(EncodeShot(shot), read));
	}

	SECTION("too long") {
		Shot shot = Along();
		shot.Aim.Direction = Vector3{3.0f, 0.0f, 0.0f};
		CHECK_FALSE(DecodeShot(EncodeShot(shot), read));
	}

	SECTION("a whisker off is accepted") {
		// A client computes this from a camera matrix in single precision, so
		// demanding exactness would refuse every honest shot.
		Shot shot = Along();
		shot.Aim.Direction = Vector3{1.0f - 1e-5f, 0.0f, 0.0f};
		CHECK(DecodeShot(EncodeShot(shot), read));
	}
}

TEST_CASE("a range past the ceiling is refused", "[examples][shooting]") {
	// Client-selected range is bounded.
	Shot read;

	Shot far = Along(MAXIMUM_SHOT_RANGE + 1.0f);
	CHECK_FALSE(DecodeShot(EncodeShot(far), read));

	Shot none = Along(0.0f);
	CHECK_FALSE(DecodeShot(EncodeShot(none), read));

	Shot negative = Along(-5.0f);
	CHECK_FALSE(DecodeShot(EncodeShot(negative), read));

	CHECK(DecodeShot(EncodeShot(Along(MAXIMUM_SHOT_RANGE)), read));
}

TEST_CASE("a non-finite coordinate is refused", "[examples][shooting]") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Shot read;

	Shot shot = Along();
	shot.Aim.Origin.Y = nan;
	CHECK_FALSE(DecodeShot(EncodeShot(shot), read));

	Shot ranged = Along();
	ranged.Range = nan;
	CHECK_FALSE(DecodeShot(EncodeShot(ranged), read));
}

TEST_CASE("trailing or missing bytes are refused", "[examples][shooting]") {
	std::vector<std::byte> bytes = EncodeShot(Along());
	Shot read;

	bytes.push_back(std::byte{0});
	CHECK_FALSE(DecodeShot(bytes, read));

	bytes.pop_back();
	bytes.pop_back();
	CHECK_FALSE(DecodeShot(bytes, read));

	CHECK_FALSE(DecodeShot({}, read));
}

TEST_CASE("the nearest target is hit, whatever order they arrive in", "[examples][shooting]") {
	// Order-independent nearest-hit result.
	const std::vector<Target> far_first = {Sphere(2, 20.0f), Sphere(1, 5.0f), Sphere(3, 50.0f)};
	const std::vector<Target> near_first = {Sphere(1, 5.0f), Sphere(3, 50.0f), Sphere(2, 20.0f)};

	const Hit a = NearestHit(Along(), far_first);
	const Hit b = NearestHit(Along(), near_first);

	REQUIRE(a.Struck);
	REQUIRE(b.Struck);
	CHECK(a.Entity.Id == 1);
	CHECK(b.Entity.Id == 1);
	CHECK(a.Distance == Approx(4.5f));
	CHECK(a.Distance == Approx(b.Distance));
}

TEST_CASE("a tie is broken by entity id rather than by order", "[examples][shooting]") {
	const std::vector<Target> forwards = {Sphere(7, 10.0f), Sphere(3, 10.0f)};
	const std::vector<Target> backwards = {Sphere(3, 10.0f), Sphere(7, 10.0f)};

	CHECK(NearestHit(Along(), forwards).Entity.Id == 3);
	CHECK(NearestHit(Along(), backwards).Entity.Id == 3);
}

TEST_CASE("a shot that reaches nothing misses", "[examples][shooting]") {
	const std::vector<Target> targets = {Sphere(1, 5.0f)};

	// Past its range.
	CHECK_FALSE(NearestHit(Along(2.0f), targets).Struck);

	// Beside the ray.
	Target aside = Sphere(1, 5.0f);
	aside.At = Vector3{5.0f, 10.0f, 0.0f};
	CHECK_FALSE(NearestHit(Along(), std::vector<Target>{aside}).Struck);

	// Behind the shooter, which is where a ray does not go.
	CHECK_FALSE(NearestHit(Along(), std::vector<Target>{Sphere(1, -5.0f)}).Struck);

	CHECK_FALSE(NearestHit(Along(), {}).Struck);
}

TEST_CASE("a shooter inside a target hits it at zero rather than behind them", "[examples][shooting]") {
	// The near intersection is negative, and reporting that would put the hit
	// behind the shooter.
	const Hit hit = NearestHit(Along(), std::vector<Target>{Sphere(1, 0.0f, 2.0f)});

	REQUIRE(hit.Struck);
	CHECK(hit.Distance == Approx(0.0f));
}

TEST_CASE("a target with no size is not a target", "[examples][shooting]") {
	Target flat = Sphere(1, 5.0f);
	flat.Radius = 0.0f;
	CHECK_FALSE(NearestHit(Along(), std::vector<Target>{flat}).Struck);

	Target broken = Sphere(1, 5.0f);
	broken.At.X = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(NearestHit(Along(), std::vector<Target>{broken}).Struck);
}

TEST_CASE("a grazing shot still counts", "[examples][shooting]") {
	// The edge of the sphere, which is where an off-by-a-sign in the
	// perpendicular test shows up.
	Target grazed = Sphere(1, 10.0f, 1.0f);
	grazed.At = Vector3{10.0f, 0.99f, 0.0f};

	CHECK(NearestHit(Along(), std::vector<Target>{grazed}).Struck);

	grazed.At = Vector3{10.0f, 1.01f, 0.0f};
	CHECK_FALSE(NearestHit(Along(), std::vector<Target>{grazed}).Struck);
}
