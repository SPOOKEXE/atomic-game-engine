#include <engine/core/types/AABB.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.aabb")
// Two corners, so every operation here is Vector3 arithmetic. Nothing else: a
// box no longer knows what a rotation is, and `OrientedBoxBounds` is covered
// beside `CFrame`, which is where it is declared.
TEST_DEPENDS("engine.core.types.vector3")

using Catch::Approx;
using engine::core::AABB;
using engine::core::Vector3;

namespace {
	// A unit cube at the origin, which is the shape most of these are about.
	const AABB UnitCube = AABB::FromCentre(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f});
}

TEST_CASE("a default box is a single point at the origin", "[aabb]") {
	const AABB degenerate;

	REQUIRE(degenerate.Minimum == Vector3::Zero);
	REQUIRE(degenerate.Maximum == Vector3::Zero);
	REQUIRE(degenerate.Size() == Vector3::Zero);
	REQUIRE(degenerate.Contains(Vector3::Zero));
}

TEST_CASE("FromCentre takes a half extent and not a size", "[aabb]") {
	// The mistake this catches makes every box in a scene twice the size it
	// should be, which reads as a physics tuning problem rather than as units.
	const AABB box = AABB::FromCentre(Vector3{10.0f, 0.0f, 0.0f}, Vector3{2.0f, 3.0f, 4.0f});

	REQUIRE(box.Minimum == Vector3{8.0f, -3.0f, -4.0f});
	REQUIRE(box.Maximum == Vector3{12.0f, 3.0f, 4.0f});
	REQUIRE(box.Size() == Vector3{4.0f, 6.0f, 8.0f});
	REQUIRE(box.Centre() == Vector3{10.0f, 0.0f, 0.0f});
}

TEST_CASE("boxes sharing only a face overlap", "[aabb]") {
	// Inclusive on purpose. An exclusive test separates a resting stack for one
	// tick whenever a contact lands exactly on a boundary, and the resulting
	// shiver is not reproducible on demand.
	const AABB left{Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}};
	const AABB right{Vector3{1.0f, 0.0f, 0.0f}, Vector3{2.0f, 1.0f, 1.0f}};

	REQUIRE(left.Overlaps(right));
	REQUIRE(right.Overlaps(left));

	// And an edge, and a corner: one shared point is still a shared point.
	const AABB corner{Vector3{1.0f, 1.0f, 1.0f}, Vector3{2.0f, 2.0f, 2.0f}};
	REQUIRE(left.Overlaps(corner));
}

TEST_CASE("a gap on any one axis is a miss", "[aabb]") {
	// All three axes, because a copy-paste that tests X three times passes
	// every other case in this file.
	const AABB origin = UnitCube;

	REQUIRE_FALSE(origin.Overlaps(AABB::FromCentre(Vector3{2.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})));
	REQUIRE_FALSE(origin.Overlaps(AABB::FromCentre(Vector3{0.0f, 2.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})));
	REQUIRE_FALSE(origin.Overlaps(AABB::FromCentre(Vector3{0.0f, 0.0f, 2.0f}, Vector3{0.5f, 0.5f, 0.5f})));

	// Negative side too, so a comparison with the operands the wrong way round
	// on one axis does not slip through.
	REQUIRE_FALSE(origin.Overlaps(AABB::FromCentre(Vector3{-2.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})));
	REQUIRE_FALSE(origin.Overlaps(AABB::FromCentre(Vector3{0.0f, -2.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})));
	REQUIRE_FALSE(origin.Overlaps(AABB::FromCentre(Vector3{0.0f, 0.0f, -2.0f}, Vector3{0.5f, 0.5f, 0.5f})));
}

TEST_CASE("a box contains its own surface", "[aabb]") {
	REQUIRE(UnitCube.Contains(Vector3::Zero));
	REQUIRE(UnitCube.Contains(Vector3{0.5f, 0.5f, 0.5f}));
	REQUIRE(UnitCube.Contains(Vector3{-0.5f, 0.0f, 0.0f}));
	REQUIRE_FALSE(UnitCube.Contains(Vector3{0.51f, 0.0f, 0.0f}));
	REQUIRE_FALSE(UnitCube.Contains(Vector3{0.0f, 0.0f, -0.51f}));
}

TEST_CASE("ClosestPoint clamps to the face rather than to the centre", "[aabb]") {
	// The shortcut this catches - returning the centre, or clamping only the
	// axis that is furthest out - makes a sphere overlap test pass for spheres
	// nowhere near the box.
	const AABB box{Vector3{0.0f, 0.0f, 0.0f}, Vector3{10.0f, 10.0f, 10.0f}};

	// Off one face only: the other two components survive untouched.
	REQUIRE(box.ClosestPoint(Vector3{-5.0f, 3.0f, 7.0f}) == Vector3{0.0f, 3.0f, 7.0f});
	REQUIRE(box.ClosestPoint(Vector3{20.0f, 3.0f, 7.0f}) == Vector3{10.0f, 3.0f, 7.0f});

	// Past a corner on all three axes at once.
	REQUIRE(box.ClosestPoint(Vector3{-1.0f, -1.0f, -1.0f}) == Vector3::Zero);

	// Already inside comes back unchanged, which is what makes the distance to
	// it zero rather than the distance to a face.
	REQUIRE(box.ClosestPoint(Vector3{4.0f, 5.0f, 6.0f}) == Vector3{4.0f, 5.0f, 6.0f});
}

TEST_CASE("a union covers both boxes and nothing more", "[aabb]") {
	const AABB left{Vector3{-1.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 2.0f}};
	const AABB right{Vector3{5.0f, -3.0f, 1.0f}, Vector3{6.0f, 0.0f, 3.0f}};

	const AABB both = left.Union(right);

	REQUIRE(both.Minimum == Vector3{-1.0f, -3.0f, 0.0f});
	REQUIRE(both.Maximum == Vector3{6.0f, 1.0f, 3.0f});

	// Symmetric, and a box unioned with itself is itself.
	REQUIRE(right.Union(left) == both);
	REQUIRE(left.Union(left) == left);
}

TEST_CASE("equality is exact and compares both corners", "[aabb]") {
	const AABB box{Vector3{1.0f, 2.0f, 3.0f}, Vector3{4.0f, 5.0f, 6.0f}};

	REQUIRE(box == AABB{Vector3{1.0f, 2.0f, 3.0f}, Vector3{4.0f, 5.0f, 6.0f}});
	REQUIRE_FALSE(box == AABB{Vector3{1.0f, 2.0f, 3.1f}, Vector3{4.0f, 5.0f, 6.0f}});
	REQUIRE_FALSE(box == AABB{Vector3{1.0f, 2.0f, 3.0f}, Vector3{4.0f, 5.1f, 6.0f}});
}

TEST_CASE("a box built the wrong way round overlaps nothing", "[aabb]") {
	// Nothing validates the corners, so this pins what an inverted box does:
	// it fails every test rather than matching everything, which is what makes
	// the mistake visible at the first query instead of the tenth.
	const AABB inverted{Vector3{1.0f, 1.0f, 1.0f}, Vector3{-1.0f, -1.0f, -1.0f}};

	REQUIRE_FALSE(inverted.Overlaps(UnitCube));
	REQUIRE_FALSE(inverted.Contains(Vector3::Zero));
}
