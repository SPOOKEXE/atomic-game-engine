#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_SUITE_ID("engine.physics.shapes")
// Every bound here is a core::AABB and two of the three are built by its own
// OrientedBoxBounds and FromCentre.
TEST_DEPENDS("engine.core.types.aabb")
// A shape is placed by a CFrame, and the cylinder bound is a function of the
// frame's up vector.
TEST_DEPENDS("engine.core.types.cframe")
// ShapeKind and Collider::Extent are scene's, and the meaning of Extent is the
// contract this suite pins.
TEST_DEPENDS("engine.scene.components")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::physics::ShapeHalfExtent;
using engine::physics::ShapeWorldBounds;
using engine::scene::Collider;
using engine::scene::ShapeKind;

namespace {
	constexpr float QUARTER_TURN = 0.7853981634f; // 45 degrees, in radians.

	Collider Of(ShapeKind shape, const Vector3 &extent) {
		Collider collider;
		collider.Shape = shape;
		collider.Extent = extent;
		return collider;
	}

	// How far a box reaches from its centre, on each axis.
	Vector3 ReachOf(const AABB &box) {
		return box.Size() * 0.5f;
	}
}

TEST_CASE("extent is a half-extent in the shape's own definition", "[physics][shapes]") {
	// The single most likely silent factor of two in the module. A box a metre
	// across has an extent of 0.5 on that axis - the same number
	// `scene::Bounds::HalfExtent` stores and the number `MakePart` halves
	// `PartDesc::Size` into.
	CHECK(ShapeHalfExtent(ShapeKind::Box, Vector3{0.5f, 1.0f, 2.0f}) == Vector3{0.5f, 1.0f, 2.0f});

	// A sphere is one number and Y and Z are not read. Reading three would make
	// an ellipsoid out of whatever the author happened to leave there, which is
	// exactly the shape of the bug: it compiles and it is nearly right.
	CHECK(ShapeHalfExtent(ShapeKind::Sphere, Vector3{2.0f, 99.0f, -7.0f}) == Vector3{2.0f, 2.0f, 2.0f});

	// A cylinder is radius in X and half-height in Y, about local Y. Z is not
	// read and takes the radius.
	CHECK(ShapeHalfExtent(ShapeKind::Cylinder, Vector3{1.5f, 4.0f, 99.0f}) == Vector3{1.5f, 4.0f, 1.5f});

	// A capsule is a segment swept by a sphere. Y is the segment half-length,
	// so the sphere radius extends the total reach at both ends.
	CHECK(ShapeHalfExtent(ShapeKind::Capsule, Vector3{1.5f, 4.0f, 99.0f}) == Vector3{1.5f, 5.5f, 1.5f});
}

TEST_CASE("the aabb derivation reads extent the same way the shape does", "[physics][shapes]") {
	// The test that fails if one of the two ever starts treating `Extent` as a
	// full extent. Unrotated, the world bound of a shape must be exactly the
	// shape's own half-extent about its position - so the two readings of
	// `Extent` are compared against each other rather than against a literal
	// that would have to be updated alongside whichever one changed.
	const Vector3 position{3.0f, -2.0f, 11.0f};
	const CFrame frame{position};

	const Collider shapes[] = {
		Of(ShapeKind::Box, Vector3{0.5f, 1.0f, 2.0f}),
		Of(ShapeKind::Sphere, Vector3{2.0f, 99.0f, -7.0f}),
		Of(ShapeKind::Cylinder, Vector3{1.5f, 4.0f, 99.0f}),
		Of(ShapeKind::Capsule, Vector3{1.5f, 4.0f, 99.0f}),
	};

	for (const Collider &collider : shapes) {
		const Vector3 expected = ShapeHalfExtent(collider.Shape, collider.Extent);
		const Vector3 reach = ReachOf(ShapeWorldBounds(collider, frame));

		CHECK(reach.X == Approx(expected.X));
		CHECK(reach.Y == Approx(expected.Y));
		CHECK(reach.Z == Approx(expected.Z));

		// And it is centred where the shape is, not at the origin.
		CHECK(ShapeWorldBounds(collider, frame).Centre().X == Approx(position.X));
		CHECK(ShapeWorldBounds(collider, frame).Centre().Y == Approx(position.Y));
		CHECK(ShapeWorldBounds(collider, frame).Centre().Z == Approx(position.Z));
	}
}

TEST_CASE("a rotated box bounds larger than an unrotated one", "[physics][shapes]") {
	// The rotate-the-centre-only bug. Turning a unit cube 45 degrees about Y
	// makes it root two wide on X and Z; a derivation that rotated the position
	// and kept the extent would return the same box it started with, which is
	// *smaller* than the shape - and a broad phase whose bound is too small
	// drops contacts and reports nothing at all.
	const Collider box = Of(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f});

	const Vector3 upright = ReachOf(ShapeWorldBounds(box, CFrame{}));
	const Vector3 turned = ReachOf(ShapeWorldBounds(box, CFrame::Angles(0.0f, QUARTER_TURN, 0.0f)));

	CHECK(turned.X > upright.X);
	CHECK(turned.Z > upright.Z);
	CHECK(turned.X == Approx(0.5f * std::sqrt(2.0f)));
	CHECK(turned.Z == Approx(0.5f * std::sqrt(2.0f)));

	// Y is the rotation axis, so it does not move. A derivation that grew every
	// axis would pass the two checks above and still be wrong.
	CHECK(turned.Y == Approx(upright.Y));
}

TEST_CASE("a rotated sphere bounds exactly as it did", "[physics][shapes]") {
	// The reason spheres are not routed through the oriented-box bound. That
	// path would return a box root two wider on two axes for a shape that
	// cannot change when it turns, and the looseness is paid once per collider
	// per tick in candidate pairs the narrow phase then throws away.
	const Collider sphere = Of(ShapeKind::Sphere, Vector3{2.0f, 0.0f, 0.0f});

	const AABB upright = ShapeWorldBounds(sphere, CFrame{});
	const AABB turned = ShapeWorldBounds(sphere, CFrame::Angles(0.3f, QUARTER_TURN, 1.1f));

	CHECK(ReachOf(turned).X == Approx(ReachOf(upright).X));
	CHECK(ReachOf(turned).Y == Approx(ReachOf(upright).Y));
	CHECK(ReachOf(turned).Z == Approx(ReachOf(upright).Z));
	CHECK(ReachOf(turned).X == Approx(2.0f));
}

TEST_CASE("a tilted cylinder bounds larger than an upright one", "[physics][shapes]") {
	// Radius 1, half-height 4: tall and thin, so tipping it is a large change
	// and a derivation that ignored the rotation is unmissable.
	const Collider cylinder = Of(ShapeKind::Cylinder, Vector3{1.0f, 4.0f, 0.0f});

	const Vector3 upright = ReachOf(ShapeWorldBounds(cylinder, CFrame{}));
	CHECK(upright.X == Approx(1.0f));
	CHECK(upright.Y == Approx(4.0f));
	CHECK(upright.Z == Approx(1.0f));

	// Laid on its side about Z, the barrel now runs along world X.
	const Vector3 onSide =
		ReachOf(ShapeWorldBounds(cylinder, CFrame::Angles(0.0f, 0.0f, QUARTER_TURN * 2.0f)));
	CHECK(onSide.X > upright.X);
	CHECK(onSide.X == Approx(4.0f));
	CHECK(onSide.Y == Approx(1.0f));
	CHECK(onSide.Z == Approx(1.0f));

	// At 45 degrees the exact bound is halfHeight * |a.e| + radius *
	// sqrt(1 - (a.e)^2) per axis, which is strictly tighter than the box around
	// the cylinder - the box bound would give 5 on X and Y where the true reach
	// is a little over 3.5. Being tighter is the whole reason this case is not
	// routed through OrientedBoxBounds.
	const Vector3 tilted = ReachOf(ShapeWorldBounds(cylinder, CFrame::Angles(0.0f, 0.0f, QUARTER_TURN)));
	const float root = std::sqrt(0.5f);
	CHECK(tilted.X == Approx(4.0f * root + 1.0f * root));
	CHECK(tilted.Y == Approx(4.0f * root + 1.0f * root));
	CHECK(tilted.X > upright.X);
	CHECK(tilted.X < 5.0f);
}

TEST_CASE("an upright cylinder bounds to its own radius, not its diagonal", "[physics][shapes]") {
	// The sqrt term is what makes a cylinder standing on its end bound to
	// exactly its radius across. A cheaper derivation that used the barrel's
	// bounding box would agree here only by accident of axis alignment, so this
	// is the case that separates the two once the frame is turned about Y -
	// spinning a cylinder about its own axis must change nothing.
	const Collider cylinder = Of(ShapeKind::Cylinder, Vector3{1.0f, 4.0f, 0.0f});

	const Vector3 upright = ReachOf(ShapeWorldBounds(cylinder, CFrame{}));
	const Vector3 spun = ReachOf(ShapeWorldBounds(cylinder, CFrame::Angles(0.0f, QUARTER_TURN, 0.0f)));

	CHECK(spun.X == Approx(upright.X));
	CHECK(spun.Y == Approx(upright.Y));
	CHECK(spun.Z == Approx(upright.Z));
}

TEST_CASE("a capsule bound follows its segment and spherical ends", "[physics][shapes]") {
	const Collider capsule = Of(ShapeKind::Capsule, Vector3{1.0f, 3.0f, 0.0f});

	const Vector3 upright = ReachOf(ShapeWorldBounds(capsule, CFrame{}));
	CHECK(upright.X == Approx(1.0f));
	CHECK(upright.Y == Approx(4.0f));
	CHECK(upright.Z == Approx(1.0f));

	const Vector3 onSide =
		ReachOf(ShapeWorldBounds(capsule, CFrame::Angles(0.0f, 0.0f, QUARTER_TURN * 2.0f)));
	CHECK(onSide.X == Approx(4.0f));
	CHECK(onSide.Y == Approx(1.0f));
	CHECK(onSide.Z == Approx(1.0f));
}

TEST_CASE("a negative extent is left alone rather than clamped", "[physics][shapes]") {
	// `core::AABB` documents a box built the wrong way round as overlapping
	// nothing, which is the answer that makes an authoring mistake visible at
	// the first test rather than at the tenth. Clamping here would turn it into
	// a shape that is silently the wrong size and collides anyway.
	const Collider broken = Of(ShapeKind::Sphere, Vector3{-1.0f, 0.0f, 0.0f});
	const AABB bound = ShapeWorldBounds(broken, CFrame{});

	CHECK(bound.Minimum.X > bound.Maximum.X);

	// It does not even contain the point it is centred on, and it misses every
	// box near it - which is what makes the mistake show up on the first test
	// somebody writes rather than as a collider that is quietly the wrong size.
	CHECK_FALSE(bound.Contains(Vector3::Zero));
	CHECK_FALSE(bound.Overlaps(AABB{Vector3{2.0f, 2.0f, 2.0f}, Vector3{3.0f, 3.0f, 3.0f}}));
}
