#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

// Private: forcing a bucket collision needs the hash, and no public caller has
// any business knowing which bucket a cell lands in.
#include "GridInternals.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.spatial.query")
// Every query is a walk of the grid, and the walk decides what a query can
// possibly see.
TEST_DEPENDS("engine.spatial.hashgrid")
// Rays and hits come from here, and RayHit has no suite of its own - this is
// where it is covered.
TEST_DEPENDS("engine.core.types.ray")
// Candidates are boxes and the exact tests are box arithmetic.
TEST_DEPENDS("engine.core.types.aabb")

using Catch::Approx;
using engine::core::AABB;
using engine::core::Ray;
using engine::core::RayHit;
using engine::core::Vector3;
using engine::spatial::GridInternals;
using engine::spatial::HashGrid;
using engine::spatial::LayerMask;
using engine::spatial::OverlapBox;
using engine::spatial::OverlapSphere;
using engine::spatial::Proxy;
using engine::spatial::QueryResult;
using engine::spatial::Raycast;
using engine::spatial::RaycastAll;
using engine::spatial::RayReciprocal;
using engine::spatial::ShapeCast;

namespace {
	// One metre cells, so a cell coordinate and a world coordinate are the same
	// number and every case below reads without arithmetic.
	constexpr float UNIT_CELL = 1.0f;

	Proxy
	Cube(uint64_t id, const Vector3 &centre, float halfExtent = 0.4f, LayerMask layers = LayerMask::All()) {
		return Proxy{id, AABB::FromCentre(centre, Vector3{halfExtent, halfExtent, halfExtent}), layers};
	}

	std::vector<uint64_t> AllIds(const HashGrid &grid, const AABB &box, LayerMask mask = LayerMask::All()) {
		std::vector<uint64_t> storage(32, 0);
		const QueryResult result = OverlapBox(grid, box, mask, storage);
		REQUIRE_FALSE(result.Overflowed);
		storage.resize(result.Written);
		return storage;
	}
}

TEST_CASE("a raycast finds the box in its way", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{3.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	const std::optional<RayHit> hit = Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 10.0f);

	REQUIRE(hit.has_value());
	REQUIRE(hit->Id == 1u);
	REQUIRE(hit->Distance == Approx(2.5f));
	REQUIRE(hit->Position.X == Approx(3.0f));
	REQUIRE(hit->Normal == Vector3{-1.0f, 0.0f, 0.0f});
}

TEST_CASE("a raycast finds nothing when there is nothing in the way", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{3.5f, 8.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	// Nothing, rather than a hit that says it is not one.
	REQUIRE_FALSE(Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 10.0f).has_value());
}

TEST_CASE("the nearest box wins, not the first cell walked", "[query]") {
	// Written when the walk was ascending on every axis and knew nothing about
	// the ray's direction, which put the *near* box in the cell reached last.
	// The ray walk reaches it first now, so what this pins today is the other
	// half of the same rule: firing backwards along an axis has to step the
	// negative way, and a walk that only ever counted up would find the far box
	// and stop at it. An implementation that returns the first candidate it
	// finds passes every single-box case in this file and fails here.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{1.5f, 0.5f, 0.5f}, 0.5f), // far from the ray's origin, first in the walk
		Cube(2, Vector3{6.5f, 0.5f, 0.5f}, 0.5f), // near the ray's origin, last in the walk
	};
	grid.Rebuild(proxies);

	const std::optional<RayHit> hit = Raycast(grid, Ray{Vector3{9.5f, 0.5f, 0.5f}, -Vector3::XAxis}, 20.0f);

	REQUIRE(hit.has_value());
	REQUIRE(hit->Id == 2u);
	REQUIRE(hit->Distance == Approx(2.5f));
	REQUIRE(hit->Normal == Vector3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("the normal is the face the ray entered through", "[query]") {
	// One case per axis and per sign. A sign error on one axis is invisible in
	// every other case, and a normal pointing into the surface reflects a
	// bounce the wrong way - which looks like a solver bug.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{0.0f, 0.0f, 0.0f}, 1.0f)};
	grid.Rebuild(proxies);

	struct Approach {
		Vector3 Origin;
		Vector3 Direction;
		Vector3 ExpectedNormal;
	};
	const std::array<Approach, 6> approaches = {{
		{Vector3{-5.0f, 0.0f, 0.0f}, Vector3::XAxis, Vector3{-1.0f, 0.0f, 0.0f}},
		{Vector3{5.0f, 0.0f, 0.0f}, -Vector3::XAxis, Vector3{1.0f, 0.0f, 0.0f}},
		{Vector3{0.0f, -5.0f, 0.0f}, Vector3::YAxis, Vector3{0.0f, -1.0f, 0.0f}},
		{Vector3{0.0f, 5.0f, 0.0f}, -Vector3::YAxis, Vector3{0.0f, 1.0f, 0.0f}},
		{Vector3{0.0f, 0.0f, -5.0f}, Vector3::ZAxis, Vector3{0.0f, 0.0f, -1.0f}},
		{Vector3{0.0f, 0.0f, 5.0f}, -Vector3::ZAxis, Vector3{0.0f, 0.0f, 1.0f}},
	}};

	for (const Approach &approach : approaches) {
		const std::optional<RayHit> hit = Raycast(grid, Ray{approach.Origin, approach.Direction}, 10.0f);
		REQUIRE(hit.has_value());
		REQUIRE(hit->Normal == approach.ExpectedNormal);
		REQUIRE(hit->Distance == Approx(4.0f));
	}
}

TEST_CASE("a ray lying exactly in the plane of a face does not become a NaN", "[query]") {
	// A ground check straight down, standing exactly on the far edge of a floor
	// tile. Nothing about that is exotic and it is the case that breaks.
	//
	// `-Vector3::YAxis` is `(-0, -1, -0)`: negating zero gives negative zero, so
	// the X and Z reciprocals are *minus* infinity. On the axis where the origin
	// sits exactly on the maximum plane the numerator is zero, zero times
	// infinity is a NaN, and a NaN compares false in both directions - so the
	// slab is read as (+inf, NaN), the entry distance becomes infinite, and the
	// floor under the character's feet is reported as a miss.
	//
	// The parallel branch is what stops that, and this is the case that fails
	// without it. Deleting it passes every other raycast in this file.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Proxy{1, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{10.0f, 1.0f, 10.0f}}, LayerMask::All()},
	};
	grid.Rebuild(proxies);

	for (const float edge : {0.0f, 10.0f}) {
		const std::optional<RayHit> onEdge =
			Raycast(grid, Ray{Vector3{edge, 5.0f, 5.0f}, -Vector3::YAxis}, 20.0f);
		REQUIRE(onEdge.has_value());
		REQUIRE_FALSE(std::isnan(onEdge->Distance));
		REQUIRE(onEdge->Distance == Approx(4.0f));
		REQUIRE(onEdge->Normal == Vector3{0.0f, 1.0f, 0.0f});
	}

	// Sideways along a face plane, which is the same arithmetic with the
	// parallel axes swapped.
	const std::optional<RayHit> grazing =
		Raycast(grid, Ray{Vector3{-5.0f, 1.0f, 10.0f}, Vector3::XAxis}, 20.0f);
	REQUIRE(grazing.has_value());
	REQUIRE(grazing->Distance == Approx(5.0f));
	REQUIRE(grazing->Normal == Vector3{-1.0f, 0.0f, 0.0f});

	// And a hair outside the plane is a clean miss rather than an accidental
	// hit, which is the half a NaN would also get wrong.
	REQUIRE_FALSE(Raycast(grid, Ray{Vector3{10.001f, 5.0f, 5.0f}, -Vector3::YAxis}, 20.0f).has_value());
	REQUIRE_FALSE(Raycast(grid, Ray{Vector3{-5.0f, 1.001f, 10.0f}, Vector3::XAxis}, 20.0f).has_value());
}

TEST_CASE("a ray starting inside a box reports it at zero distance", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	const std::optional<RayHit> hit = Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 10.0f);

	// Here, not the far face: a query asking what it is touching wants the
	// answer "you are in it".
	REQUIRE(hit.has_value());
	REQUIRE(hit->Distance == 0.0f);
	REQUIRE(hit->Position == Vector3{0.5f, 0.5f, 0.5f});
}

TEST_CASE("a box beyond the distance asked for is not a hit", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{8.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	const Ray ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis};
	REQUIRE_FALSE(Raycast(grid, ray, 5.0f).has_value());
	REQUIRE(Raycast(grid, ray, 9.0f).has_value());
}

TEST_CASE("a box behind the origin is not a hit", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	REQUIRE_FALSE(Raycast(grid, Ray{Vector3{5.5f, 0.5f, 0.5f}, Vector3::XAxis}, 20.0f).has_value());
}

TEST_CASE("a ray with no direction and a distance of zero find nothing", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	// Both refused before the grid is consulted. A direction-less ray has no
	// line to test and a distance of zero travels nowhere.
	REQUIRE_FALSE(Raycast(grid, Ray{}, 10.0f).has_value());
	REQUIRE_FALSE(Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 0.0f).has_value());
	REQUIRE_FALSE(Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, -1.0f).has_value());
}

TEST_CASE("a raycast honours the layer mask", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{2.5f, 0.5f, 0.5f}, 0.5f, LayerMask::Only(0)),
		Cube(2, Vector3{5.5f, 0.5f, 0.5f}, 0.5f, LayerMask::Only(1)),
	};
	grid.Rebuild(proxies);

	const Ray ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis};

	REQUIRE(Raycast(grid, ray, 20.0f)->Id == 1u);
	REQUIRE(Raycast(grid, ray, 20.0f, LayerMask::Only(1))->Id == 2u);
	REQUIRE_FALSE(Raycast(grid, ray, 20.0f, LayerMask::Only(5)).has_value());
}

TEST_CASE("RaycastAll reports every box, nearest first", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{2.5f, 0.5f, 0.5f}, 0.5f),
		Cube(2, Vector3{5.5f, 0.5f, 0.5f}, 0.5f),
		Cube(3, Vector3{8.5f, 0.5f, 0.5f}, 0.5f),
	};
	grid.Rebuild(proxies);

	std::array<RayHit, 8> hits{};
	const QueryResult result =
		RaycastAll(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 20.0f, LayerMask::All(), hits);

	REQUIRE(result.Written == 3);
	REQUIRE_FALSE(result.Overflowed);
	REQUIRE(hits[0].Id == 1u);
	REQUIRE(hits[1].Id == 2u);
	REQUIRE(hits[2].Id == 3u);
	REQUIRE(hits[0].Distance < hits[1].Distance);
	REQUIRE(hits[1].Distance < hits[2].Distance);
}

TEST_CASE("a full span reports overflow and keeps the nearest", "[query]") {
	// The boxes are laid out so the walk meets the furthest first: firing
	// backwards makes the nearest the last candidate found, so an
	// implementation that stops writing once the span is full keeps exactly the
	// wrong two.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{1.5f, 0.5f, 0.5f}, 0.5f),
		Cube(2, Vector3{4.5f, 0.5f, 0.5f}, 0.5f),
		Cube(3, Vector3{7.5f, 0.5f, 0.5f}, 0.5f),
	};
	grid.Rebuild(proxies);

	std::array<RayHit, 2> hits{};
	const QueryResult result =
		RaycastAll(grid, Ray{Vector3{9.5f, 0.5f, 0.5f}, -Vector3::XAxis}, 20.0f, LayerMask::All(), hits);

	REQUIRE(result.Written == 2);
	REQUIRE(result.Overflowed);
	REQUIRE(hits[0].Id == 3u);
	REQUIRE(hits[1].Id == 2u);
}

TEST_CASE("a span exactly the size of the answer does not report overflow", "[query]") {
	// The case `Written` alone cannot distinguish from a truncated one, which
	// is the whole reason the flag is reported rather than inferred.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{2.5f, 0.5f, 0.5f}, 0.5f),
		Cube(2, Vector3{5.5f, 0.5f, 0.5f}, 0.5f),
	};
	grid.Rebuild(proxies);

	std::array<RayHit, 2> hits{};
	const QueryResult result =
		RaycastAll(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 20.0f, LayerMask::All(), hits);

	REQUIRE(result.Written == 2);
	REQUIRE_FALSE(result.Overflowed);

	std::array<uint64_t, 2> ids{};
	const QueryResult overlap =
		OverlapBox(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{9.0f, 1.0f, 1.0f}}, LayerMask::All(), ids);
	REQUIRE(overlap.Written == 2);
	REQUIRE_FALSE(overlap.Overflowed);
}

TEST_CASE("a zero-length span writes nothing and reports overflow", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{2.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	const std::span<uint64_t> nowhere;
	const QueryResult overlap = OverlapBox(
		grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{9.0f, 1.0f, 1.0f}}, LayerMask::All(), nowhere
	);
	REQUIRE(overlap.Written == 0);
	REQUIRE(overlap.Overflowed);

	const std::span<RayHit> noHits;
	const QueryResult cast =
		RaycastAll(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 20.0f, LayerMask::All(), noHits);
	REQUIRE(cast.Written == 0);
	REQUIRE(cast.Overflowed);

	// And with nothing to find, an empty span has not overflowed - it held
	// everything there was.
	const QueryResult empty = OverlapBox(
		grid, AABB{Vector3{40.0f, 40.0f, 40.0f}, Vector3{41.0f, 41.0f, 41.0f}}, LayerMask::All(), nowhere
	);
	REQUIRE_FALSE(empty.Overflowed);
}

TEST_CASE("a bucket collision still rejects the box", "[query]") {
	// A candidate arriving from a cell that merely hashed to the same bucket
	// must be thrown out. The cell check catches it and so does the box test;
	// both are here because removing either leaves a query returning things
	// that are nowhere near it.
	HashGrid grid{UNIT_CELL};
	const Proxy seed[] = {Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.4f)};
	grid.Rebuild(seed);

	const size_t target = GridInternals::BucketOf(grid, 0, 0, 0);
	int32_t collidingX = 0;
	for (int32_t candidate = 1; candidate < 100000; candidate++) {
		if (GridInternals::BucketOf(grid, candidate, 0, 0) == target) {
			collidingX = candidate;
			break;
		}
	}
	REQUIRE(collidingX != 0);

	const float offset = static_cast<float>(collidingX);
	const Proxy proxies[] = {
		Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.4f),
		Cube(2, Vector3{offset + 0.5f, 0.5f, 0.5f}, 0.4f),
	};
	grid.Rebuild(proxies);

	REQUIRE(
		AllIds(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}}) == std::vector<uint64_t>{1}
	);
}

TEST_CASE("OverlapBox counts a shared face as an overlap", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Proxy{1, AABB{Vector3{1.0f, 0.0f, 0.0f}, Vector3{2.0f, 1.0f, 1.0f}}, LayerMask::All()}
	};
	grid.Rebuild(proxies);

	// Touching, not intersecting. An exclusive test separates a resting stack
	// for a tick whenever a contact lands on a boundary.
	REQUIRE(
		AllIds(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}}) == std::vector<uint64_t>{1}
	);

	// And a hair short of touching is not.
	REQUIRE(AllIds(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.99f, 1.0f, 1.0f}}).empty());
}

TEST_CASE("OverlapSphere measures to the nearest point of the box", "[query]") {
	// Not to its centre. A large box beside a small sphere is in contact long
	// before their centres are close, and clamping to the centre is the
	// shortcut that makes a trigger fire from across the room.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Proxy{1, AABB{Vector3{2.0f, 0.0f, 0.0f}, Vector3{12.0f, 1.0f, 1.0f}}, LayerMask::All()}
	};
	grid.Rebuild(proxies);

	std::array<uint64_t, 4> found{};

	// The near face is two metres away; the centre is seven.
	REQUIRE(OverlapSphere(grid, Vector3{0.0f, 0.5f, 0.5f}, 2.5f, LayerMask::All(), found).Written == 1);
	REQUIRE(OverlapSphere(grid, Vector3{0.0f, 0.5f, 0.5f}, 1.5f, LayerMask::All(), found).Written == 0);

	// A sphere reaching a corner diagonally, which is the case a per-axis test
	// gets wrong: 3-4-5 puts the corner exactly five away.
	REQUIRE(OverlapSphere(grid, Vector3{-1.0f, 5.0f, 0.5f}, 5.01f, LayerMask::All(), found).Written == 1);
	REQUIRE(OverlapSphere(grid, Vector3{-1.0f, 5.0f, 0.5f}, 4.99f, LayerMask::All(), found).Written == 0);
}

TEST_CASE("a sphere of zero radius is a point and a negative one finds nothing", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	std::array<uint64_t, 4> found{};
	REQUIRE(OverlapSphere(grid, Vector3{0.5f, 0.5f, 0.5f}, 0.0f, LayerMask::All(), found).Written == 1);
	REQUIRE(OverlapSphere(grid, Vector3{5.5f, 0.5f, 0.5f}, 0.0f, LayerMask::All(), found).Written == 0);

	// A negative radius is a caller mistake with no sensible answer, so it gets
	// the empty one rather than being squared into a positive.
	const QueryResult negative =
		OverlapSphere(grid, Vector3{0.5f, 0.5f, 0.5f}, -3.0f, LayerMask::All(), found);
	REQUIRE(negative.Written == 0);
	REQUIRE_FALSE(negative.Overflowed);
}

TEST_CASE("a shape cast finds what the box passes through", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{5.5f, 0.5f, 0.5f}, 0.5f),
		Cube(2, Vector3{5.5f, 6.5f, 0.5f}, 0.5f), // beside the path, never touched
	};
	grid.Rebuild(proxies);

	std::array<uint64_t, 8> found{};
	const AABB start = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.25f, 0.25f, 0.25f});
	const QueryResult result = ShapeCast(grid, start, Vector3{9.0f, 0.0f, 0.0f}, LayerMask::All(), found);

	REQUIRE(result.Written == 1);
	REQUIRE(found[0] == 1u);
}

TEST_CASE("a shape cast rejects the corner its swept bounds cover", "[query]") {
	// The union of the start and end boxes is a much bigger volume than the
	// sweep. A cast that tested only that box would report the corner it never
	// reaches, and a character would stop at a doorway it fits through
	// diagonally.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{9.5f, 9.5f, 0.5f}, 0.4f)};
	grid.Rebuild(proxies);

	std::array<uint64_t, 8> found{};
	const AABB start = AABB::FromCentre(Vector3{0.5f, 9.5f, 0.5f}, Vector3{0.25f, 0.25f, 0.25f});

	// Diagonally down and across: the swept bounds cover the corner box, and
	// the sweep itself passes well below it.
	const QueryResult diagonal = ShapeCast(grid, start, Vector3{9.0f, -9.0f, 0.0f}, LayerMask::All(), found);
	REQUIRE(diagonal.Written == 0);

	// Straight at it, and it is found.
	const QueryResult direct = ShapeCast(grid, start, Vector3{9.0f, 0.0f, 0.0f}, LayerMask::All(), found);
	REQUIRE(direct.Written == 1);
	REQUIRE(found[0] == 1u);
}

TEST_CASE("the swept-box walk visits the thick line and not its bounding corners", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{32.5f, 32.5f, 0.5f}, 0.2f),
		Cube(2, Vector3{32.5f, 33.2f, 0.5f}, 0.2f), // touched by the box, not its centre line
		Cube(3, Vector3{60.5f, 2.5f, 0.5f}, 0.2f),	// inside the swept bound, far from the path
		Proxy{4, AABB{Vector3{-60.0f, 32.0f, -60.0f}, Vector3{60.0f, 33.0f, 60.0f}}, LayerMask::All()},
		Cube(5, Vector3{40.5f, 40.5f, 0.5f}, 0.2f, LayerMask::Only(1)),
	};
	grid.Rebuild(proxies);

	const AABB start = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.6f, 0.6f, 0.6f});
	const Vector3 motion{64.0f, 64.0f, 0.0f};
	const float distance = motion.Magnitude();
	const Ray ray{start.Centre(), motion / distance};
	const RayReciprocal reciprocal{ray.Direction};
	const AABB swept = start.Union(AABB{start.Minimum + motion, start.Maximum + motion});

	std::array<int, 6> visits{};
	const bool completed = GridInternals::ForEachCandidateAlongSweptBox(
		grid,
		ray,
		reciprocal,
		distance,
		start.Size() * 0.5f,
		swept,
		LayerMask::Only(0),
		[&](const Proxy &proxy) {
			visits[proxy.Id]++;
			return true;
		}
	);

	REQUIRE(completed);
	REQUIRE(visits[1] == 1);
	REQUIRE(visits[2] == 1);
	REQUIRE(visits[3] == 0);
	REQUIRE(visits[4] == 1);
	REQUIRE(visits[5] == 0);

	std::array<uint64_t, 6> found{};
	const QueryResult result = ShapeCast(grid, start, motion, LayerMask::Only(0), found);
	REQUIRE(result.Written == 3);
	REQUIRE_FALSE(result.Overflowed);
	std::array<bool, 6> returned{};
	for (size_t index = 0; index < result.Written; index++) {
		returned[found[index]] = true;
	}
	REQUIRE(returned[1]);
	REQUIRE(returned[2]);
	REQUIRE_FALSE(returned[3]);
	REQUIRE(returned[4]);
	REQUIRE_FALSE(returned[5]);
}

TEST_CASE("a swept box reports a multi-cell proxy once in either direction", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{32.5f, 32.5f, 0.5f}, 2.5f)};
	grid.Rebuild(proxies);

	std::array<uint64_t, 8> forward{};
	const AABB fromLeft = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.5f, 0.5f, 0.5f});
	const QueryResult forwardResult =
		ShapeCast(grid, fromLeft, Vector3{64.0f, 64.0f, 0.0f}, LayerMask::All(), forward);
	REQUIRE(forwardResult.Written == 1);
	REQUIRE_FALSE(forwardResult.Overflowed);
	REQUIRE(forward[0] == 1u);

	std::array<uint64_t, 8> backward{};
	const AABB fromRight = AABB::FromCentre(Vector3{64.5f, 64.5f, 0.5f}, Vector3{0.5f, 0.5f, 0.5f});
	const QueryResult backwardResult =
		ShapeCast(grid, fromRight, Vector3{-64.0f, -64.0f, 0.0f}, LayerMask::All(), backward);
	REQUIRE(backwardResult.Written == 1);
	REQUIRE_FALSE(backwardResult.Overflowed);
	REQUIRE(backward[0] == 1u);
}

TEST_CASE("a swept box longer than the cell walk is worth takes the scan", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{3.5f, 3.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	std::array<uint64_t, 4> found{};
	const AABB start = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.25f, 0.25f, 0.25f});
	const QueryResult result =
		ShapeCast(grid, start, Vector3{100000.0f, 100000.0f, 0.0f}, LayerMask::All(), found);

	REQUIRE(result.Written == 1);
	REQUIRE_FALSE(result.Overflowed);
	REQUIRE(found[0] == 1u);
}

TEST_CASE("a shape cast with no motion is an overlap", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.5f),
		Cube(2, Vector3{6.5f, 0.5f, 0.5f}, 0.5f),
	};
	grid.Rebuild(proxies);

	const AABB box = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.25f, 0.25f, 0.25f});

	std::array<uint64_t, 8> swept{};
	std::array<uint64_t, 8> overlapped{};
	const QueryResult castResult = ShapeCast(grid, box, Vector3::Zero, LayerMask::All(), swept);
	const QueryResult overlapResult = OverlapBox(grid, box, LayerMask::All(), overlapped);

	REQUIRE(castResult.Written == overlapResult.Written);
	REQUIRE(castResult.Written == 1);
	REQUIRE(swept[0] == overlapped[0]);
}

TEST_CASE("a shape cast reports a box it already overlaps", "[query]") {
	// Distance zero is still a hit. A cast that started inside something and
	// reported nothing would let a body already interpenetrating keep moving.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	std::array<uint64_t, 4> found{};
	const AABB box = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.1f, 0.1f, 0.1f});
	REQUIRE(ShapeCast(grid, box, Vector3{5.0f, 0.0f, 0.0f}, LayerMask::All(), found).Written == 1);
}

TEST_CASE("a shape cast that outgrows the span keeps a walk-order prefix", "[query]") {
	// The same `Overflowed` contract as the overlaps, pinned for the sweep:
	// the span holds a prefix, the flag says there was more, and the prefix
	// is the walk's own order - which for a straight sweep is along the path.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{2.5f, 0.5f, 0.5f}, 0.5f),
		Cube(2, Vector3{4.5f, 0.5f, 0.5f}, 0.5f),
		Cube(3, Vector3{6.5f, 0.5f, 0.5f}, 0.5f),
	};
	grid.Rebuild(proxies);

	std::array<uint64_t, 2> found{};
	const AABB start = AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.25f, 0.25f, 0.25f});
	const QueryResult result = ShapeCast(grid, start, Vector3{9.0f, 0.0f, 0.0f}, LayerMask::All(), found);

	REQUIRE(result.Written == 2);
	REQUIRE(result.Overflowed);
	REQUIRE(found[0] == 1u);
	REQUIRE(found[1] == 2u);
}

TEST_CASE("every query answers an empty grid with nothing", "[query]") {
	const HashGrid grid{UNIT_CELL};
	std::array<uint64_t, 4> ids{};
	std::array<RayHit, 4> hits{};
	const AABB anywhere{Vector3{-5.0f, -5.0f, -5.0f}, Vector3{5.0f, 5.0f, 5.0f}};

	REQUIRE_FALSE(Raycast(grid, Ray{Vector3::Zero, Vector3::XAxis}, 100.0f).has_value());
	REQUIRE(
		RaycastAll(grid, Ray{Vector3::Zero, Vector3::XAxis}, 100.0f, LayerMask::All(), hits).Written == 0
	);
	REQUIRE(OverlapBox(grid, anywhere, LayerMask::All(), ids).Written == 0);
	REQUIRE(OverlapSphere(grid, Vector3::Zero, 50.0f, LayerMask::All(), ids).Written == 0);
	REQUIRE(ShapeCast(grid, anywhere, Vector3{1.0f, 0.0f, 0.0f}, LayerMask::All(), ids).Written == 0);
}

TEST_CASE("a coarse proxy is found by every query", "[query]") {
	// The hierarchy is a second route through every walk, and a query that
	// forgot it would miss the floor of every world.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Proxy{1, AABB{Vector3{-60.0f, -1.0f, -60.0f}, Vector3{60.0f, 0.0f, 60.0f}}, LayerMask::All()}
	};
	grid.Rebuild(proxies);
	REQUIRE(GridInternals::OversizedCount(grid) == 0);
	REQUIRE(GridInternals::LevelProxyCount(grid, 2) == 1);

	std::array<uint64_t, 4> ids{};
	std::array<RayHit, 4> hits{};

	const std::optional<RayHit> down =
		Raycast(grid, Ray{Vector3{30.0f, 5.0f, 30.0f}, -Vector3::YAxis}, 20.0f);
	REQUIRE(down.has_value());
	REQUIRE(down->Distance == Approx(5.0f));
	REQUIRE(down->Normal == Vector3{0.0f, 1.0f, 0.0f});

	REQUIRE(
		RaycastAll(grid, Ray{Vector3{30.0f, 5.0f, 30.0f}, -Vector3::YAxis}, 20.0f, LayerMask::All(), hits)
			.Written == 1
	);
	REQUIRE(
		OverlapBox(grid, AABB::FromCentre(Vector3{30.0f, 0.0f, 30.0f}, Vector3::One), LayerMask::All(), ids)
			.Written == 1
	);
	REQUIRE(OverlapSphere(grid, Vector3{30.0f, 1.0f, 30.0f}, 2.0f, LayerMask::All(), ids).Written == 1);
	REQUIRE(
		ShapeCast(
			grid,
			AABB::FromCentre(Vector3{30.0f, 5.0f, 30.0f}, Vector3{0.5f, 0.5f, 0.5f}),
			Vector3{0.0f, -10.0f, 0.0f},
			LayerMask::All(),
			ids
		)
			.Written == 1
	);
}

TEST_CASE("a residual proxy is found exactly once by every query", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Proxy{
			1,
			AABB{Vector3{-1000000.0f, -3.0f, -1000000.0f}, Vector3{1000000.0f, -2.0f, 1000000.0f}},
			LayerMask::Only(1),
		},
		Cube(2, Vector3{0.5f, -8.0f, 0.5f}, 0.4f, LayerMask::Only(1)),
	};
	grid.Rebuild(proxies);
	REQUIRE(GridInternals::OversizedCount(grid) == 1);

	const Ray down{Vector3{0.5f, 0.5f, 0.5f}, -Vector3::YAxis};
	const std::optional<RayHit> nearest = Raycast(grid, down, 20.0f, LayerMask::Only(1));
	REQUIRE(nearest.has_value());
	REQUIRE(nearest->Id == 1u);
	REQUIRE(nearest->Distance == Approx(2.5f));
	REQUIRE_FALSE(Raycast(grid, down, 20.0f, LayerMask::Only(0)).has_value());

	std::array<RayHit, 4> hits{};
	const QueryResult all = RaycastAll(grid, down, 20.0f, LayerMask::Only(1), hits);
	REQUIRE(all.Written == 2);
	REQUIRE(hits[0].Id == 1u);
	REQUIRE(hits[1].Id == 2u);

	std::array<uint64_t, 4> ids{};
	const AABB floorBox = AABB::FromCentre(Vector3{0.5f, -2.5f, 0.5f}, Vector3{0.5f, 0.5f, 0.5f});
	REQUIRE(OverlapBox(grid, floorBox, LayerMask::Only(1), ids).Written == 1);
	REQUIRE(ids[0] == 1u);
	REQUIRE(OverlapSphere(grid, Vector3{0.5f, -1.5f, 0.5f}, 1.0f, LayerMask::Only(1), ids).Written == 1);
	REQUIRE(ids[0] == 1u);
	REQUIRE(
		ShapeCast(
			grid,
			AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.5f, 0.5f, 0.5f}),
			Vector3{0.0f, -5.0f, 0.0f},
			LayerMask::Only(1),
			ids
		)
			.Written == 1
	);
	REQUIRE(ids[0] == 1u);
}

TEST_CASE("a proxy spanning cells the ray crosses is reported once", "[query]") {
	// The ray walk's de-duplication rule: report a proxy from the first cell of
	// the unbroken run the ray makes through its cell range, which is the cell
	// whose predecessor in the walk was outside that range.
	//
	// The box below is five cells wide on X and the ray goes straight down the
	// middle of it, so a walk with no rule at all reports it five times. The
	// box walk's rule cannot answer this: its designated cell is a corner of a
	// *volume*, and a line has none.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{5.5f, 0.5f, 0.5f}, 2.5f)};
	grid.Rebuild(proxies);

	std::array<RayHit, 8> hits{};
	const QueryResult all =
		RaycastAll(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 20.0f, LayerMask::All(), hits);

	REQUIRE(all.Written == 1);
	REQUIRE_FALSE(all.Overflowed);
	REQUIRE(hits[0].Id == 1u);
	REQUIRE(hits[0].Distance == Approx(2.5f));
}

TEST_CASE("a ray starting inside a multi-cell proxy reports it once", "[query]") {
	// The run starts at the walk's very first cell, where there is no previous
	// cell to compare against. "No previous" has to count as outside the range,
	// or the one case where the ray begins inside something reports nothing.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{4.5f, 0.5f, 0.5f}, 3.5f)};
	grid.Rebuild(proxies);

	std::array<RayHit, 8> hits{};
	const QueryResult all =
		RaycastAll(grid, Ray{Vector3{2.5f, 0.5f, 0.5f}, Vector3::XAxis}, 20.0f, LayerMask::All(), hits);

	REQUIRE(all.Written == 1);
	REQUIRE(hits[0].Id == 1u);
	REQUIRE(hits[0].Distance == Approx(0.0f));
}

TEST_CASE("a diagonal ray finds what is on the line and not what is beside it", "[query]") {
	// The reason the walk exists. Both boxes sit inside the bounding box of the
	// segment, so the volume walk handed both to the exact test; only one is on
	// the line, and only that one is now visited at all. The answer was already
	// right - what changed is how many cells were opened to reach it.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{10.5f, 10.5f, 0.5f}, 0.4f), // on the diagonal
		Cube(2, Vector3{18.0f, 2.0f, 0.5f}, 0.4f),	// in the corner of the segment's box
	};
	grid.Rebuild(proxies);

	const Vector3 diagonal = Vector3{1.0f, 1.0f, 0.0f}.Unit();
	std::array<RayHit, 8> hits{};
	const QueryResult all =
		RaycastAll(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, diagonal}, 40.0f, LayerMask::All(), hits);

	REQUIRE(all.Written == 1);
	REQUIRE(hits[0].Id == 1u);
}

TEST_CASE("the nearest hit includes a coarse proxy", "[query]") {
	// `Raycast` stops walking once it holds a hit no later cell can beat, and
	// a coarse proxy is in a different cell walk, so the stop must not take the
	// pass that finds one with it. The floor here is nearer than the cube, and
	// the cube is what the walk finds first.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{0.5f, -8.0f, 0.5f}, 0.4f),
		Proxy{2, AABB{Vector3{-60.0f, -3.0f, -60.0f}, Vector3{60.0f, -2.0f, 60.0f}}, LayerMask::All()},
	};
	grid.Rebuild(proxies);
	REQUIRE(GridInternals::OversizedCount(grid) == 0);
	REQUIRE(GridInternals::LevelProxyCount(grid, 2) == 1);

	const std::optional<RayHit> hit = Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, -Vector3::YAxis}, 20.0f);

	REQUIRE(hit.has_value());
	REQUIRE(hit->Id == 2u);
	REQUIRE(hit->Distance == Approx(2.5f));
}

TEST_CASE("a ray longer than the walk is worth takes the scan", "[query]") {
	// Past `WALK_CELL_ALLOWANCE` cells the walk costs more than one pass over
	// every proxy, and the fallback has to give the same answer rather than a
	// cheaper one. A hundred thousand metres at one metre a cell is well past
	// it on a grid holding a single box.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Cube(1, Vector3{3.5f, 0.5f, 0.5f}, 0.5f)};
	grid.Rebuild(proxies);

	const std::optional<RayHit> hit =
		Raycast(grid, Ray{Vector3{0.5f, 0.5f, 0.5f}, Vector3::XAxis}, 100000.0f);

	REQUIRE(hit.has_value());
	REQUIRE(hit->Id == 1u);
	REQUIRE(hit->Distance == Approx(2.5f));
}

TEST_CASE("the overlap queries honour the layer mask", "[query]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Cube(1, Vector3{0.5f, 0.5f, 0.5f}, 0.4f, LayerMask::Only(0)),
		Cube(2, Vector3{1.5f, 0.5f, 0.5f}, 0.4f, LayerMask::Only(3)),
	};
	grid.Rebuild(proxies);

	const AABB both{Vector3{0.0f, 0.0f, 0.0f}, Vector3{2.0f, 1.0f, 1.0f}};
	std::array<uint64_t, 4> found{};

	REQUIRE(OverlapBox(grid, both, LayerMask::Only(3), found).Written == 1);
	REQUIRE(found[0] == 2u);
	REQUIRE(OverlapSphere(grid, Vector3{1.0f, 0.5f, 0.5f}, 2.0f, LayerMask::Only(0), found).Written == 1);
	REQUIRE(found[0] == 1u);
	REQUIRE(
		ShapeCast(
			grid,
			AABB::FromCentre(Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.1f, 0.1f, 0.1f}),
			Vector3{2.0f, 0.0f, 0.0f},
			LayerMask::None(),
			found
		)
			.Written == 0
	);
}
