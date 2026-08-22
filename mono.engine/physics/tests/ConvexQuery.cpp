#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Suite.hpp>

// Private: GJK and EPA are the narrow phase's own machinery and nothing outside
// this module has a use for either.
#include "ContactPairs.hpp"
#include "ConvexQuery.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.physics.convexquery")
// The exact pairs are the oracle every case here is checked against.
TEST_DEPENDS("engine.physics.narrowphase")
TEST_DEPENDS("engine.core.types.cframe")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::physics::ClosestPoints;
using engine::physics::ContactBetween;
using engine::physics::ContactSolution;
using engine::physics::ConvexPenetration;
using engine::physics::ConvexSeparation;
using engine::physics::ConvexSweep;
using engine::physics::PenetrationBetween;
using engine::physics::ShapeInstance;
using engine::physics::SweepConvex;
using engine::scene::ShapeKind;

namespace {
	ShapeInstance Box(const Vector3 &at, const Vector3 &half, const CFrame &turn = CFrame{}) {
		return ShapeInstance{CFrame{at} * turn, half, ShapeKind::Box};
	}

	ShapeInstance Sphere(const Vector3 &at, float radius) {
		return ShapeInstance{CFrame{at}, Vector3{radius, radius, radius}, ShapeKind::Sphere};
	}
}

TEST_CASE("two separated boxes report the gap between their faces", "[convexquery]") {
	// The arithmetic is checkable by hand, which is what a first case should be:
	// unit cubes four metres apart centre to centre are three metres apart face
	// to face.
	const ShapeInstance left = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance right = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});

	const ConvexSeparation gap = ClosestPoints(left, right);
	CHECK_FALSE(gap.Overlapping);
	CHECK(gap.Distance == Approx(3.0f).margin(1e-3f));
	CHECK(gap.OnFirst.X == Approx(0.5f).margin(1e-3f));
	CHECK(gap.OnSecond.X == Approx(3.5f).margin(1e-3f));
}

TEST_CASE("two separated spheres report the gap between their surfaces", "[convexquery]") {
	// The case that fails if the search direction reaches `SupportPoint`
	// unnormalised: a sphere's support is `centre + direction * radius`, so a
	// direction of length three is a sphere three times too big and the gap
	// comes out negative.
	const ShapeInstance left = Sphere(Vector3{0.0f, 0.0f, 0.0f}, 1.0f);
	const ShapeInstance right = Sphere(Vector3{10.0f, 0.0f, 0.0f}, 2.0f);

	const ConvexSeparation gap = ClosestPoints(left, right);
	CHECK_FALSE(gap.Overlapping);
	CHECK(gap.Distance == Approx(7.0f).margin(1e-3f));
	CHECK(gap.OnFirst.X == Approx(1.0f).margin(1e-3f));
	CHECK(gap.OnSecond.X == Approx(8.0f).margin(1e-3f));
}

TEST_CASE("a diagonal gap is the distance between two corners", "[convexquery]") {
	// A closest feature that is a vertex on both shapes, which is the simplex
	// case a search that only ever reduces to a face gets wrong.
	const ShapeInstance left = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance right = Box(Vector3{3.0f, 3.0f, 3.0f}, Vector3{0.5f, 0.5f, 0.5f});

	const ConvexSeparation gap = ClosestPoints(left, right);
	CHECK_FALSE(gap.Overlapping);

	const Vector3 corner{0.5f, 0.5f, 0.5f};
	const Vector3 other{2.5f, 2.5f, 2.5f};
	CHECK(gap.Distance == Approx((other - corner).Magnitude()).margin(1e-3f));
}

TEST_CASE("overlapping shapes say so rather than reporting a distance", "[convexquery]") {
	// GJK answers "are they apart, and by how much". A caller wanting a depth
	// asks the other function, and the split is what keeps the cheap query cheap.
	const ShapeInstance left = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const ShapeInstance right = Box(Vector3{1.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});

	CHECK(ClosestPoints(left, right).Overlapping);
	CHECK(ClosestPoints(left, left).Overlapping);
}

TEST_CASE("the penetration of two boxes is the overlap along the shallowest axis", "[convexquery]") {
	// **The normal points from the first shape toward the second**, which is the
	// convention every pair function in this module obeys and the one thing a
	// general algorithm can get backwards without anything failing to build -
	// the symptom is two shapes that suck together instead of pushing apart.
	const ShapeInstance left = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const ShapeInstance right = Box(Vector3{1.5f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});

	const ConvexPenetration hit = PenetrationBetween(left, right);
	REQUIRE(hit.Overlapping);
	CHECK(hit.Depth == Approx(0.5f).margin(1e-2f));
	CHECK(hit.Normal.X == Approx(1.0f).margin(1e-2f));
}

TEST_CASE("the penetration of two spheres is what the analytic pair says", "[convexquery]") {
	// Sphere against sphere is exact and closed form, so any disagreement here
	// is the general algorithm being wrong rather than the oracle being
	// approximate.
	const ShapeInstance left = Sphere(Vector3{0.0f, 0.0f, 0.0f}, 1.0f);
	const ShapeInstance right = Sphere(Vector3{1.5f, 0.0f, 0.0f}, 1.0f);

	const ContactSolution exact = ContactBetween(left, right);
	REQUIRE(exact.Touching);

	const ConvexPenetration hit = PenetrationBetween(left, right);
	REQUIRE(hit.Overlapping);
	CHECK(hit.Depth == Approx(exact.Penetrations[0]).margin(1e-2f));
	CHECK(hit.Normal.Dot(exact.Normal) > 0.99f);
}

TEST_CASE("the general answer agrees with the exact one over many placements", "[convexquery]") {
	// **The oracle case, and the reason box-box was worth keeping exact.** A
	// separating-axis search over two boxes is closed form; a Minkowski-difference
	// search is not, and running them against each other over a sweep of
	// placements is what says the general one is right rather than plausible.
	const ShapeInstance anchor = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.75f, 0.5f});

	size_t compared = 0;
	for (int step = 1; step < 18; step++) {
		const auto offset = static_cast<float>(step) * 0.1f;
		const ShapeInstance moving = Box(Vector3{offset, 0.0f, 0.0f}, Vector3{1.0f, 0.75f, 0.5f});

		const ContactSolution exact = ContactBetween(anchor, moving);
		const ConvexPenetration hit = PenetrationBetween(anchor, moving);
		REQUIRE(exact.Touching == hit.Overlapping);
		if (!hit.Overlapping) {
			continue;
		}

		compared++;
		INFO("offset " << offset);

		// **The depth is the assertion; the axis is only one when it is
		// unique.** These are two boxes of the same size, so the overlaps on the
		// three axes are `2 - offset`, 1.5 and 1.0 - and wherever two of those
		// are equal there are two different minimum translations and a search is
		// free to return either. At an offset of one metre the x and z overlaps
		// are both exactly a metre, and the two implementations returned
		// perpendicular normals of the same length. Both are right.
		//
		// The sign is never asserted, for the same reason one step further: a
		// pair overlapping completely on an axis separates by pushing either way
		// along it.
		float overlaps[3] = {2.0f - offset, 1.5f, 1.0f};
		std::sort(std::begin(overlaps), std::end(overlaps));
		const bool unique = overlaps[1] - overlaps[0] > 1e-3f;
		if (unique) {
			CHECK(std::abs(hit.Normal.Dot(exact.Normal)) > 0.99f);
		}

		// The deepest point of the exact manifold is the same overlap the
		// general search reports, because both are the minimum translation -
		// and that holds whether or not the axis was unique.
		float deepest = 0.0f;
		for (size_t point = 0; point < exact.PointCount; point++) {
			deepest = std::max(deepest, exact.Penetrations[point]);
		}
		CHECK(hit.Depth == Approx(deepest).margin(1e-2f));
	}
	CHECK(compared > 5);
}

TEST_CASE("a turned box is handled as well as an axis-aligned one", "[convexquery]") {
	// A rotation is where a general search earns its keep, and it is also where
	// a support function that forgot to rotate the direction stops being wrong
	// in a way an axis-aligned case would show.
	const ShapeInstance anchor = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const CFrame turned = CFrame::Angles(0.0f, 0.7854f, 0.0f);
	const ShapeInstance tilted = Box(Vector3{2.2f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}, turned);

	const ContactSolution exact = ContactBetween(anchor, tilted);
	const ConvexPenetration hit = PenetrationBetween(anchor, tilted);
	REQUIRE(exact.Touching == hit.Overlapping);

	if (hit.Overlapping) {
		CHECK(hit.Normal.Dot(exact.Normal) > 0.98f);
	}

	// And well apart, the gap is the distance between the two nearest faces.
	const ShapeInstance far = Box(Vector3{6.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}, turned);
	const ConvexSeparation gap = ClosestPoints(anchor, far);
	CHECK_FALSE(gap.Overlapping);
	CHECK(gap.Distance > 3.0f);
	CHECK(gap.Distance < 5.0f);
}

TEST_CASE("two shapes that only touch overlap at no depth", "[convexquery]") {
	// The honest answer, and it is what keeps a body resting exactly on a
	// surface from flickering between "apart" and "in".
	const ShapeInstance left = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const ShapeInstance right = Box(Vector3{2.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});

	const ConvexPenetration hit = PenetrationBetween(left, right);
	if (hit.Overlapping) {
		CHECK(hit.Depth == Approx(0.0f).margin(1e-2f));
	} else {
		CHECK(ClosestPoints(left, right).Distance == Approx(0.0f).margin(1e-2f));
	}
}

TEST_CASE("two shapes far apart never say they overlap", "[convexquery]") {
	// The direction the failure has to run in. A missed contact for one tick is
	// a body sinking a millimetre; an invented one at a hundred metres is a
	// body thrown across the map.
	const ShapeInstance left = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f});

	for (int step = 1; step < 40; step++) {
		const auto offset = static_cast<float>(step) * 3.7f;
		const ShapeInstance right = Sphere(Vector3{offset, offset * 0.3f, -offset * 0.7f}, 0.9f);
		CHECK_FALSE(PenetrationBetween(left, right).Overlapping);
		CHECK_FALSE(ClosestPoints(left, right).Overlapping);
	}
}

TEST_CASE("a shape against itself is the deepest overlap there is", "[convexquery]") {
	// Two coincident shapes make the Minkowski difference centred on the origin,
	// which is the case every stage of both searches divides by something in.
	const ShapeInstance box = Box(Vector3{3.0f, -2.0f, 1.0f}, Vector3{1.0f, 1.0f, 1.0f});

	const ConvexPenetration hit = PenetrationBetween(box, box);
	REQUIRE(hit.Overlapping);
	CHECK(hit.Depth == Approx(2.0f).margin(1e-2f));
	CHECK(hit.Normal.Magnitude() == Approx(1.0f).margin(1e-3f));
}

TEST_CASE("a sweep stops at the surface rather than in it", "[convexquery]") {
	// **The case continuous collision exists for.** A metre cube travelling
	// twenty metres in one step, at a thin wall four metres along: stepping the
	// transform puts it out the far side, and the narrow phase next tick finds
	// nothing because there is nothing left to find.
	const ShapeInstance bullet = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance wall = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.05f, 5.0f, 5.0f});

	const ConvexSweep hit = SweepConvex(bullet, Vector3{20.0f, 0.0f, 0.0f}, wall);
	REQUIRE(hit.Hit);

	// It touches when its leading face reaches the wall's near face: the cube's
	// centre is at 4 - 0.05 - 0.5 = 3.45, which is 3.45 of the 20 metres.
	CHECK(hit.Fraction == Approx(3.45f / 20.0f).margin(2e-3f));

	// And the normal pushes it back the way it came.
	CHECK(hit.Normal.X == Approx(-1.0f).margin(1e-2f));
}

TEST_CASE("a sweep that misses reports no hit", "[convexquery]") {
	// Past the wall's edge, and moving parallel to it. Both are the answers a
	// caller acts on by leaving the body where the integrator put it.
	const ShapeInstance high = Box(Vector3{0.0f, 20.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance wall = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.05f, 1.0f, 1.0f});

	CHECK_FALSE(SweepConvex(high, Vector3{20.0f, 0.0f, 0.0f}, wall).Hit);

	const ShapeInstance beside = Box(Vector3{0.0f, 3.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	CHECK_FALSE(SweepConvex(beside, Vector3{0.0f, 0.0f, 20.0f}, wall).Hit);
}

TEST_CASE("a sweep that stops short of the wall reports no hit", "[convexquery]") {
	// The boundary of the feature. A body that does not reach is a body the
	// ordinary narrow phase handles next tick, and clamping it early would be a
	// body that stops in mid-air.
	const ShapeInstance bullet = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance wall = Box(Vector3{20.0f, 0.0f, 0.0f}, Vector3{0.05f, 5.0f, 5.0f});

	CHECK_FALSE(SweepConvex(bullet, Vector3{5.0f, 0.0f, 0.0f}, wall).Hit);
}

TEST_CASE("a sweep of a shape already touching reports a hit at zero", "[convexquery]") {
	// What keeps a caller from stepping a body further into whatever it is
	// already inside.
	const ShapeInstance inside = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance wall = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.05f, 5.0f, 5.0f});

	const ConvexSweep hit = SweepConvex(inside, Vector3{20.0f, 0.0f, 0.0f}, wall);
	REQUIRE(hit.Hit);
	CHECK(hit.Fraction == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("a sweep with no motion is an overlap question", "[convexquery]") {
	// Answered here rather than by dividing by a zero travel, so a caller does
	// not have to ask two different functions depending on how fast something
	// is going.
	const ShapeInstance touching = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance wall = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.05f, 5.0f, 5.0f});
	CHECK(SweepConvex(touching, Vector3::Zero, wall).Hit);

	const ShapeInstance apart = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	CHECK_FALSE(SweepConvex(apart, Vector3::Zero, wall).Hit);
}

TEST_CASE("a sweep against a sphere lands where the geometry says", "[convexquery]") {
	// A curved target, so the answer is not a face plane the search could have
	// found by accident. A unit sphere at ten metres and a small box arriving
	// along the axis: contact when the box's leading face reaches nine metres.
	const ShapeInstance pellet = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.1f, 0.1f, 0.1f});
	const ShapeInstance ball = Sphere(Vector3{10.0f, 0.0f, 0.0f}, 1.0f);

	const ConvexSweep hit = SweepConvex(pellet, Vector3{30.0f, 0.0f, 0.0f}, ball);
	REQUIRE(hit.Hit);
	CHECK(hit.Fraction == Approx(8.9f / 30.0f).margin(3e-3f));
}

TEST_CASE("a sweep never steps past the contact it was called to find", "[convexquery]") {
	// **The property conservative advancement has and a fixed-step search does
	// not.** Every advance is a lower bound on the time of impact, so the answer
	// approaches from before the contact - which means the body is clamped short
	// of the surface rather than a fraction of a millimetre inside it.
	const ShapeInstance wall = Box(Vector3{4.0f, 0.0f, 0.0f}, Vector3{0.25f, 5.0f, 5.0f});

	for (int step = 0; step < 24; step++) {
		const auto height = static_cast<float>(step) * 0.13f - 1.5f;
		const ShapeInstance bullet = Box(Vector3{0.0f, height, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
		const Vector3 motion{18.0f, 0.0f, 0.0f};

		const ConvexSweep hit = SweepConvex(bullet, motion, wall);
		REQUIRE(hit.Hit);

		// Placed at the reported fraction, the shapes must not be overlapping -
		// which is what "clamped short" means and what the caller relies on.
		const ShapeInstance stopped =
			Box(Vector3{motion.X * hit.Fraction, height, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
		CHECK_FALSE(PenetrationBetween(stopped, wall).Depth > 1e-3f);
	}
}

// --- sweeping a triangle mesh -------------------------------------------------
//
// **A soup is not convex, so conservative advancement has no answer for one on
// its own.** `Support` reports zero reach for `ShapeKind::Mesh`, which makes a
// mesh behave as a single point at its own frame - so a sweep against terrain
// answered about a point in the middle of it and a bullet went through a
// hillside as if it were not there.
//
// **Every case here puts the triangles somewhere the frame is not**, which is
// what separates the walk from the point it replaced: a mesh whose geometry sits
// on its own origin gives the same answer either way, and the whole failure was
// that the geometry is elsewhere.

namespace {
	// A flat floor of two triangles, four metres square, `height` above the
	// mesh's own origin.
	engine::collision::TriangleMesh Floor(float height) {
		const std::vector<Vector3> points{
			Vector3{-2.0f, height, -2.0f},
			Vector3{2.0f, height, -2.0f},
			Vector3{-2.0f, height, 2.0f},
			Vector3{2.0f, height, 2.0f},
		};
		const std::vector<uint32_t> indices{0, 2, 1, 1, 2, 3};
		return engine::collision::BuildTriangleMesh(points, indices);
	}

	// A wall of two triangles in the YZ plane, `x` from the mesh's own origin.
	engine::collision::TriangleMesh Wall(float x) {
		const std::vector<Vector3> points{
			Vector3{x, -2.0f, -2.0f},
			Vector3{x, 2.0f, -2.0f},
			Vector3{x, -2.0f, 2.0f},
			Vector3{x, 2.0f, 2.0f},
		};
		const std::vector<uint32_t> indices{0, 2, 1, 1, 2, 3};
		return engine::collision::BuildTriangleMesh(points, indices);
	}

	ShapeInstance MeshAt(const engine::collision::TriangleMesh &mesh, const Vector3 &at) {
		return ShapeInstance{
			CFrame(at), Vector3{0.5f, 0.5f, 0.5f}, engine::scene::ShapeKind::Mesh, nullptr, &mesh
		};
	}
}

TEST_CASE("a sweep stops at a triangle and not at the mesh's origin", "[convexquery]") {
	// The wall's triangles stand six metres along X from the mesh's own frame,
	// which is at the origin. A metre cube travelling twenty metres has to stop
	// at the triangles - a version that answered about the frame would stop at
	// half a metre.
	const engine::collision::TriangleMesh wall = Wall(6.0f);
	const ShapeInstance fixed = MeshAt(wall, Vector3::Zero);
	const ShapeInstance bullet = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});

	const ConvexSweep hit = SweepConvex(bullet, Vector3{20.0f, 0.0f, 0.0f}, fixed);
	REQUIRE(hit.Hit);

	// A triangle has no thickness, so the leading face reaches the plane when
	// the centre is at 5.5 - which is 5.5 of the twenty metres.
	CHECK(hit.Fraction == Approx(5.5f / 20.0f).margin(3e-3f));
	CHECK(hit.Normal.X == Approx(-1.0f).margin(5e-2f));
}

TEST_CASE("a sweep lands on a triangle mesh floor", "[convexquery]") {
	// The case a character is: falling onto ground that is a soup rather than a
	// slab. The floor is three metres below the mesh's frame.
	const engine::collision::TriangleMesh floor = Floor(-3.0f);
	const ShapeInstance ground = MeshAt(floor, Vector3::Zero);
	const ShapeInstance falling = Box(Vector3{0.0f, 6.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});

	const ConvexSweep hit = SweepConvex(falling, Vector3{0.0f, -20.0f, 0.0f}, ground);
	REQUIRE(hit.Hit);

	// Its underside meets the plane when the centre is at -2.5, which is 8.5 of
	// the twenty metres. The frame it would have stopped at is at 5.5.
	CHECK(hit.Fraction == Approx(8.5f / 20.0f).margin(3e-3f));
	CHECK(hit.Normal.Y == Approx(1.0f).margin(5e-2f));
}

TEST_CASE("a sweep through a mesh's frame with no triangle there misses", "[convexquery]") {
	// **The half that says this is a gather and not a bound.** The body passes
	// straight through the mesh's own origin and through the middle of its
	// bound, and there is no triangle within three metres of either.
	const engine::collision::TriangleMesh floor = Floor(-3.0f);
	const ShapeInstance ground = MeshAt(floor, Vector3::Zero);

	const ShapeInstance across = Box(Vector3{-8.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});
	CHECK_FALSE(SweepConvex(across, Vector3{16.0f, 0.0f, 0.0f}, ground).Hit);
}

TEST_CASE("a sweep past the edge of a mesh misses", "[convexquery]") {
	// Beyond where the triangles reach, along the plane they lie in. A bound
	// would still be four metres square here; the triangles are what answer.
	const engine::collision::TriangleMesh floor = Floor(0.0f);
	const ShapeInstance ground = MeshAt(floor, Vector3::Zero);

	const ShapeInstance beside = Box(Vector3{-8.0f, 0.0f, 6.0f}, Vector3{0.5f, 0.5f, 0.5f});
	CHECK_FALSE(SweepConvex(beside, Vector3{16.0f, 0.0f, 0.0f}, ground).Hit);
}

TEST_CASE("a sweep against a mesh takes the earliest triangle", "[convexquery]") {
	// Two walls in one mesh, four metres apart. The walk tests every triangle
	// the swept box overlaps, so the answer has to be the nearer of them rather
	// than whichever the gather listed last.
	const std::vector<Vector3> points{
		Vector3{4.0f, -2.0f, -2.0f},
		Vector3{4.0f, 2.0f, -2.0f},
		Vector3{4.0f, -2.0f, 2.0f},
		Vector3{4.0f, 2.0f, 2.0f},
		Vector3{8.0f, -2.0f, -2.0f},
		Vector3{8.0f, 2.0f, -2.0f},
		Vector3{8.0f, -2.0f, 2.0f},
		Vector3{8.0f, 2.0f, 2.0f},
	};

	// The far wall's triangles are listed *first*, so a walk that kept the last
	// hit rather than the earliest would answer with the far one.
	const std::vector<uint32_t> indices{4, 6, 5, 5, 6, 7, 0, 2, 1, 1, 2, 3};
	const engine::collision::TriangleMesh both = engine::collision::BuildTriangleMesh(points, indices);

	const ShapeInstance fixed = MeshAt(both, Vector3::Zero);
	const ShapeInstance bullet = Box(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f});

	const ConvexSweep hit = SweepConvex(bullet, Vector3{20.0f, 0.0f, 0.0f}, fixed);
	REQUIRE(hit.Hit);
	CHECK(hit.Fraction == Approx(3.5f / 20.0f).margin(3e-3f));
}
