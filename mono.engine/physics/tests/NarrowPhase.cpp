#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Suite.hpp>

// Private: the six pair functions are the unit under test here. Putting a known
// answer against one pair directly is the only way to tell a wrong normal
// inside a pair from a wrong flip in the dispatcher, and the two failures look
// identical from outside the module.
#include "ContactPairs.hpp"
#include "PipelineInternals.hpp"
#include "ShapeSupport.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>

TEST_SUITE_ID("engine.physics.narrowphase")
// The pairs it turns into manifolds, and the ordering both agree on.
TEST_DEPENDS("engine.physics.broadphase")
// The manifold and event types this step fills in.
TEST_DEPENDS("engine.physics.contacts")
// `Collider::Extent` means what `Shapes.hpp` says, and every pair function
// reads it that way.
TEST_DEPENDS("engine.physics.shapes")
// ShapeKind is closed at three, which is what makes six pairs the whole set.
TEST_DEPENDS("engine.scene.enums")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BoxBox;
using engine::physics::BoxCylinder;
using engine::physics::BoxSphere;
using engine::physics::BroadPhase;
using engine::physics::ContactBetween;
using engine::physics::ContactManifold;
using engine::physics::ContactSolution;
using engine::physics::CylinderCylinder;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::physics::PreparePhysicsWorld;
using engine::physics::ShapeInstance;
using engine::physics::SphereCylinder;
using engine::physics::SphereSphere;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::ShapeKind;
using engine::scene::Transform;

namespace {
	constexpr float QUARTER_TURN = 1.5707963268f;
	constexpr float SIXTH_TURN = 0.5235987756f; // 30 degrees

	ShapeInstance Box(const Vector3 &position, const Vector3 &extent, const CFrame &rotation = CFrame{}) {
		return ShapeInstance{CFrame{position, rotation.Rotation()}, extent, ShapeKind::Box};
	}

	ShapeInstance Sphere(const Vector3 &position, float radius) {
		return ShapeInstance{CFrame{position}, Vector3{radius, 0.0f, 0.0f}, ShapeKind::Sphere};
	}

	ShapeInstance
	Cylinder(const Vector3 &position, float radius, float halfHeight, const CFrame &rotation = CFrame{}) {
		return ShapeInstance{
			CFrame{position, rotation.Rotation()}, Vector3{radius, halfHeight, 0.0f}, ShapeKind::Cylinder
		};
	}

	ShapeInstance
	Capsule(const Vector3 &position, float radius, float halfSegment, const CFrame &rotation = CFrame{}) {
		return ShapeInstance{
			CFrame{position, rotation.Rotation()}, Vector3{radius, halfSegment, 0.0f}, ShapeKind::Capsule
		};
	}

	// The deepest penetration in a solution, which is the number a known-answer
	// case is stated in.
	float DeepestOf(const ContactSolution &solution) {
		float deepest = 0.0f;
		for (size_t index = 0; index < solution.PointCount; index++) {
			deepest = solution.Penetrations[index] > deepest ? solution.Penetrations[index] : deepest;
		}
		return deepest;
	}

	void CheckNormal(const Vector3 &normal, const Vector3 &expected) {
		CHECK(normal.X == Approx(expected.X).margin(1e-4));
		CHECK(normal.Y == Approx(expected.Y).margin(1e-4));
		CHECK(normal.Z == Approx(expected.Z).margin(1e-4));
	}

	Entity Place(Store &store, const Vector3 &position, const Collider &collider, bool moving) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{position}});
		store.Set<Collider>(entity, collider);
		if (moving) {
			store.Set<Motion>(entity, Motion{});
		}
		return entity;
	}

	Collider Shaped(ShapeKind shape, const Vector3 &extent, bool trigger = false) {
		Collider collider;
		collider.Shape = shape;
		collider.Extent = extent;
		collider.Trigger = trigger;
		return collider;
	}

	void Step(Store &store) {
		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
	}
}

TEST_CASE("capsules use the general convex contact path", "[physics][narrowphase]") {
	const ContactSolution sphereTouch =
		ContactBetween(Sphere(Vector3::Zero, 0.5f), Capsule(Vector3{0.8f, 0.0f, 0.0f}, 0.5f, 1.0f));
	REQUIRE(sphereTouch.Touching);
	CHECK(DeepestOf(sphereTouch) == Approx(0.2f).margin(2e-3));
	CHECK(sphereTouch.Normal.X > 0.999f);
	CHECK(std::abs(sphereTouch.Normal.Y) < 2e-3f);
	CHECK(std::abs(sphereTouch.Normal.Z) < 2e-3f);

	const ContactSolution capsuleTouch =
		ContactBetween(Capsule(Vector3::Zero, 0.5f, 1.0f), Capsule(Vector3{0.8f, 0.0f, 0.0f}, 0.5f, 1.0f));
	REQUIRE(capsuleTouch.Touching);
	CHECK(DeepestOf(capsuleTouch) == Approx(0.2f).margin(2e-3));

	CHECK_FALSE(
		ContactBetween(Sphere(Vector3::Zero, 0.5f), Capsule(Vector3{1.2f, 0.0f, 0.0f}, 0.5f, 1.0f)).Touching
	);
}

// --- the six, each with a known answer ---------------------------------------
//
// Stated as a depth and a normal rather than as a point count, because those
// two are what the solver acts on and what a sign mistake changes. Every case
// puts the second shape on the positive side of the first, so the expected
// normal is a positive axis in every one of the six - which makes a flipped
// pair unmissable in the diff.

TEST_CASE("box against box has a known depth and normal", "[physics][narrowphase]") {
	const ContactSolution solution = BoxBox(
		Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}),
		Box(Vector3{0.9f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::XAxis);
	CHECK(DeepestOf(solution) == Approx(0.1f).margin(1e-4));
}

TEST_CASE("box against sphere has a known depth and normal", "[physics][narrowphase]") {
	// Closest point on the box is its +X face at 0.5; the centre is 0.3 beyond
	// it, so a radius of 0.5 overlaps by 0.2.
	const ContactSolution solution =
		BoxSphere(Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}), Sphere(Vector3{0.8f, 0.0f, 0.0f}, 0.5f));

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::XAxis);
	REQUIRE(solution.PointCount == 1);
	CHECK(solution.Penetrations[0] == Approx(0.2f).margin(1e-4));

	// On the surface of the *second* shape, which is the sphere. A point on the
	// box's face instead would be 0.2 further along the normal and would read
	// as correct in every drawing of it.
	CHECK(solution.Positions[0].X == Approx(0.3f).margin(1e-4));
}

TEST_CASE("box against cylinder has a known depth and normal", "[physics][narrowphase]") {
	const ContactSolution solution = BoxCylinder(
		Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}), Cylinder(Vector3{0.0f, 0.9f, 0.0f}, 0.5f, 0.5f)
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(DeepestOf(solution) == Approx(0.1f).margin(1e-4));

	// A cap against a face is a whole disc of contact, so it must not come back
	// as one point - that is the manifold a resting cylinder cannot stand on.
	CHECK(solution.PointCount > 1);
}

TEST_CASE("sphere against sphere has a known depth and normal", "[physics][narrowphase]") {
	const ContactSolution solution =
		SphereSphere(Sphere(Vector3::Zero, 1.0f), Sphere(Vector3{1.5f, 0.0f, 0.0f}, 1.0f));

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::XAxis);
	REQUIRE(solution.PointCount == 1);
	CHECK(solution.Penetrations[0] == Approx(0.5f).margin(1e-4));
	CHECK(solution.Positions[0].X == Approx(0.5f).margin(1e-4));
}

TEST_CASE("sphere against cylinder has a known depth and normal", "[physics][narrowphase]") {
	// The sphere sits under the cylinder's flat end. Nearest point of the
	// cylinder is its bottom cap at y = 0.2, which is 0.2 from the sphere's
	// centre, so a radius of 0.5 overlaps by 0.3.
	const ContactSolution solution =
		SphereCylinder(Sphere(Vector3::Zero, 0.5f), Cylinder(Vector3{0.0f, 1.2f, 0.0f}, 1.0f, 1.0f));

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	REQUIRE(solution.PointCount == 1);
	CHECK(solution.Penetrations[0] == Approx(0.3f).margin(1e-4));
	CHECK(solution.Positions[0].Y == Approx(0.2f).margin(1e-4));
}

TEST_CASE("cylinder against cylinder has a known depth and normal", "[physics][narrowphase]") {
	const ContactSolution solution = CylinderCylinder(
		Cylinder(Vector3::Zero, 0.5f, 0.5f), Cylinder(Vector3{0.0f, 0.9f, 0.0f}, 0.5f, 0.5f)
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(DeepestOf(solution) == Approx(0.1f).margin(1e-4));
	CHECK(solution.PointCount > 1);
}

TEST_CASE("a pair that does not overlap reports not touching", "[physics][narrowphase]") {
	// **Not touching is a result and not a failure to decide.** Each of the six
	// has to be able to say no, and the separated case is the one a solution
	// left default-constructed would pass by accident - so every one of them is
	// also checked for a zero point count.
	const ContactSolution pairs[] = {
		BoxBox(
			Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}),
			Box(Vector3{2.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})
		),
		BoxSphere(Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}), Sphere(Vector3{2.0f, 0.0f, 0.0f}, 0.5f)),
		BoxCylinder(
			Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}), Cylinder(Vector3{0.0f, 3.0f, 0.0f}, 0.5f, 0.5f)
		),
		SphereSphere(Sphere(Vector3::Zero, 1.0f), Sphere(Vector3{2.5f, 0.0f, 0.0f}, 1.0f)),
		SphereCylinder(Sphere(Vector3::Zero, 0.5f), Cylinder(Vector3{0.0f, 3.0f, 0.0f}, 1.0f, 1.0f)),
		CylinderCylinder(
			Cylinder(Vector3::Zero, 0.5f, 0.5f), Cylinder(Vector3{0.0f, 3.0f, 0.0f}, 0.5f, 0.5f)
		),
	};

	for (const ContactSolution &solution : pairs) {
		CHECK_FALSE(solution.Touching);
		CHECK(solution.PointCount == 0);
	}
}

// --- the convention ----------------------------------------------------------

TEST_CASE("the dispatcher flips exactly once", "[physics][narrowphase]") {
	// `ContactBetween` takes its shapes in entity order and the pair functions
	// take theirs in shape order, so half of all pairs are called reversed.
	// Asking the same geometry both ways round has to give exactly opposite
	// normals and the same depth - a flip that also moved the points to the
	// wrong surface, or one applied twice, fails here.
	const ShapeInstance box = Box(Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f});
	const ShapeInstance sphere = Sphere(Vector3{0.8f, 0.0f, 0.0f}, 0.5f);

	const ContactSolution forward = ContactBetween(box, sphere);
	const ContactSolution reversed = ContactBetween(sphere, box);

	REQUIRE(forward.Touching);
	REQUIRE(reversed.Touching);
	CheckNormal(forward.Normal, Vector3::XAxis);
	CheckNormal(reversed.Normal, -Vector3::XAxis);
	CHECK(forward.Penetrations[0] == Approx(reversed.Penetrations[0]).margin(1e-4));

	// Points sit on the second shape either way round: on the sphere at 0.3
	// going forward, on the box's face at 0.5 coming back.
	CHECK(forward.Positions[0].X == Approx(0.3f).margin(1e-4));
	CHECK(reversed.Positions[0].X == Approx(0.5f).margin(1e-4));
}

TEST_CASE("every pair puts its points on the second shape", "[physics][narrowphase]") {
	// The relation the flip depends on: the point on the first shape is the
	// point on the second, one penetration further along the normal. If a pair
	// function reported the other surface, this is the case that says so
	// before the solver starts pushing from the wrong place.
	const ShapeInstance first = Sphere(Vector3::Zero, 1.0f);
	const ShapeInstance second = Sphere(Vector3{1.5f, 0.0f, 0.0f}, 1.0f);

	const ContactSolution solution = SphereSphere(first, second);
	REQUIRE(solution.PointCount == 1);

	const Vector3 onFirst = solution.Positions[0] + solution.Normal * solution.Penetrations[0];
	CHECK(onFirst.X == Approx(1.0f).margin(1e-4));
}

// --- the multi-point manifold -------------------------------------------------

TEST_CASE("a box resting flat on a box gives four points", "[physics][narrowphase]") {
	// **The reason box-box needs clipping at all.** One point is one
	// constraint: the box pivots about it and rocks, and the rocking never
	// damps because every tick is a fresh single constraint. Four points hold
	// it against translation and both rotations.
	const ContactSolution solution = BoxBox(
		Box(Vector3::Zero, Vector3{4.0f, 0.5f, 4.0f}),
		Box(Vector3{0.0f, 0.95f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f})
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(solution.PointCount == ContactManifold::MAXIMUM_POINTS);

	// All four at the same depth, which is what "resting flat" means and what
	// keeps the solver from tipping it.
	for (size_t index = 1; index < solution.PointCount; index++) {
		CHECK(solution.Penetrations[index] == Approx(solution.Penetrations[0]).margin(1e-4));
	}

	// And spread out rather than piled up: four points at one place is four
	// copies of one constraint.
	float widest = 0.0f;
	for (size_t index = 1; index < solution.PointCount; index++) {
		const float distance = (solution.Positions[index] - solution.Positions[0]).Magnitude();
		widest = distance > widest ? distance : widest;
	}
	CHECK(widest > 0.9f);
}

TEST_CASE("a box corner into a face gives one point", "[physics][narrowphase]") {
	// The other end of the same machinery. A box tipped onto a corner really
	// does touch at one place, and reporting four there would invent three
	// constraints - which reads as a box that refuses to topple.
	const ContactSolution solution = BoxBox(
		Box(Vector3::Zero, Vector3{4.0f, 0.5f, 4.0f}),
		Box(Vector3{0.0f, 1.32f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, CFrame::Angles(0.6f, 0.7f, 0.5f))
	);

	REQUIRE(solution.Touching);
	CHECK(solution.PointCount == 1);
}

// --- the two cylinder cases the design names ---------------------------------

TEST_CASE("the cylinder disc-edge case is one rim point", "[physics][narrowphase]") {
	// **Budgeted for, not discovered.** A tilted cylinder meets a floor on the
	// edge of its disc - not on the cap, not on the barrel - and the contact is
	// a single point of the rim. A cap-shaped answer here would plant four
	// points in the air, and the cylinder would stand up on nothing.
	//
	// Tilted 30 degrees, so the lowest rim point is
	// 2*cos(30) + 1*sin(30) = 2.2320 below the centre. Placed for a 0.05
	// overlap with a floor whose top face is at 0.5.
	const float lowest = 2.0f * std::cos(SIXTH_TURN) + 1.0f * std::sin(SIXTH_TURN);
	const ContactSolution solution = BoxCylinder(
		Box(Vector3::Zero, Vector3{5.0f, 0.5f, 5.0f}),
		Cylinder(Vector3{0.0f, 0.45f + lowest, 0.0f}, 1.0f, 2.0f, CFrame::Angles(0.0f, 0.0f, SIXTH_TURN))
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(solution.PointCount == 1);
	CHECK(solution.Penetrations[0] == Approx(0.05f).margin(1e-3));

	// The point is the rim vertex nearest the floor: on the floor's surface,
	// and exactly one radius out from the barrel's axis. A cap-shaped answer
	// would put it at the centre of the disc, which is a whole radius above the
	// floor and a whole radius off the rim.
	CHECK(solution.Positions[0].Y == Approx(0.45f).margin(1e-3));

	const Vector3 axis = Vector3{-std::sin(SIXTH_TURN), std::cos(SIXTH_TURN), 0.0f};
	const Vector3 offset = solution.Positions[0] - Vector3{0.0f, 0.45f + lowest, 0.0f};
	const Vector3 radial = offset - axis * offset.Dot(axis);
	CHECK(radial.Magnitude() == Approx(1.0f).margin(1e-3));
}

TEST_CASE("the cylinder parallel-axis case gives two points", "[physics][narrowphase]") {
	// **The degenerate case, and it is degenerate in a specific way**: two
	// parallel barrels have no unique closest pair of points, so a
	// closest-point answer picks an arbitrary one of a whole line and the
	// cylinder rolls in place forever. Clipping the overlapping stretch of the
	// two lines is what gives it two points and holds it still.
	const CFrame lying = CFrame::Angles(0.0f, 0.0f, QUARTER_TURN);
	const ContactSolution solution = CylinderCylinder(
		Cylinder(Vector3::Zero, 0.5f, 2.0f, lying), Cylinder(Vector3{0.0f, 0.9f, 0.0f}, 0.5f, 2.0f, lying)
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(solution.PointCount == 2);
	CHECK(DeepestOf(solution) == Approx(0.1f).margin(1e-4));

	// Two points at the two ends of the shared stretch, four metres apart -
	// the whole length of both barrels, because they are lying exactly on top
	// of each other.
	const float spread = (solution.Positions[1] - solution.Positions[0]).Magnitude();
	CHECK(spread == Approx(4.0f).margin(1e-3));
}

TEST_CASE("two crossed cylinders meet at one point", "[physics][narrowphase]") {
	// The other half of the barrel-barrel case, and the one the parallel branch
	// must not swallow: crossed barrels really do touch at a single place, and
	// the axis that finds it is the cross product the parallel case makes
	// degenerate.
	const ContactSolution solution = CylinderCylinder(
		Cylinder(Vector3::Zero, 0.5f, 2.0f, CFrame::Angles(0.0f, 0.0f, QUARTER_TURN)),
		Cylinder(Vector3{0.0f, 0.9f, 0.0f}, 0.5f, 2.0f, CFrame::Angles(QUARTER_TURN, 0.0f, 0.0f))
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(solution.PointCount == 1);
	CHECK(solution.Penetrations[0] == Approx(0.1f).margin(1e-3));
}

TEST_CASE("a cylinder lying on a box rests on a line", "[physics][narrowphase]") {
	// The barrel against a face. Two points, for the same reason the parallel
	// pair needs two: one point lets it roll about the contact.
	const ContactSolution solution = BoxCylinder(
		Box(Vector3::Zero, Vector3{5.0f, 0.5f, 5.0f}),
		Cylinder(Vector3{0.0f, 0.95f, 0.0f}, 0.5f, 2.0f, CFrame::Angles(0.0f, 0.0f, QUARTER_TURN))
	);

	REQUIRE(solution.Touching);
	CheckNormal(solution.Normal, Vector3::YAxis);
	CHECK(solution.PointCount == 2);
	CHECK((solution.Positions[1] - solution.Positions[0]).Magnitude() == Approx(4.0f).margin(1e-3));
}

// --- the system ---------------------------------------------------------------

TEST_CASE("the narrow phase names its bodies the way the pair did", "[physics][narrowphase]") {
	// The end-to-end version of the convention case above. The sphere is
	// created first so it holds the smaller id and is therefore `A`, while the
	// pair function takes the box first because `ShapeKind::Box` sorts before
	// `Sphere` - so this only passes if the dispatcher flipped.
	Store store("narrowphase.convention");
	PreparePhysicsWorld(store, 1.0f);

	const Entity sphere =
		Place(store, Vector3::Zero, Shaped(ShapeKind::Sphere, Vector3{0.5f, 0.0f, 0.0f}), true);
	const Entity box =
		Place(store, Vector3{0.8f, 0.0f, 0.0f}, Shaped(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f}), true);
	REQUIRE(sphere.Id < box.Id);

	Step(store);

	const auto manifolds = store.Resource<PhysicsWorld>()->Manifolds();
	REQUIRE(manifolds.size() == 1);
	CHECK(manifolds[0].A == sphere);
	CHECK(manifolds[0].B == box);

	// From A toward B, which is from the sphere toward the box: +X.
	CheckNormal(manifolds[0].Normal, Vector3::XAxis);
}

TEST_CASE("a candidate pair that does not really touch produces no manifold", "[physics][narrowphase]") {
	// The whole reason the step exists. Two spheres whose boxes overlap at the
	// corners and whose surfaces are nowhere near each other are a candidate
	// and not a contact.
	Store store("narrowphase.reject");
	PreparePhysicsWorld(store, 4.0f);

	Place(store, Vector3::Zero, Shaped(ShapeKind::Sphere, Vector3{0.5f, 0.0f, 0.0f}), true);
	Place(store, Vector3{0.9f, 0.9f, 0.0f}, Shaped(ShapeKind::Sphere, Vector3{0.5f, 0.0f, 0.0f}), true);

	Step(store);

	const PhysicsWorld &world = *store.Resource<PhysicsWorld>();
	CHECK(world.Pairs().size() == 1);
	CHECK(world.Manifolds().empty());
}

TEST_CASE("a trigger produces a manifold marked as one", "[physics][narrowphase]") {
	Store store("narrowphase.trigger");
	PreparePhysicsWorld(store, 1.0f);

	Place(store, Vector3::Zero, Shaped(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f}, true), true);
	Place(store, Vector3{0.5f, 0.0f, 0.0f}, Shaped(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f}), true);

	Step(store);

	const auto manifolds = store.Resource<PhysicsWorld>()->Manifolds();
	REQUIRE(manifolds.size() == 1);
	CHECK(manifolds[0].Trigger);
}

TEST_CASE("manifolds come out in pair order", "[physics][narrowphase]") {
	// Sequential impulse is order-dependent, so the solver's visit order is the
	// manifold list's order and the manifold list's order has to be the pair
	// list's. A step that filtered the pairs into a different sequence would
	// put two runs of one scene on different trajectories.
	Store store("narrowphase.order");
	PreparePhysicsWorld(store, 1.0f);

	for (int index = 0; index < 5; index++) {
		Place(
			store,
			Vector3{static_cast<float>(index) * 0.6f, 0.0f, 0.0f},
			Shaped(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f}),
			true
		);
	}

	Step(store);

	const PhysicsWorld &world = *store.Resource<PhysicsWorld>();
	const auto manifolds = world.Manifolds();
	REQUIRE(manifolds.size() > 1);
	for (size_t index = 1; index < manifolds.size(); index++) {
		const bool ascending = manifolds[index - 1].A.Id < manifolds[index].A.Id ||
							   (manifolds[index - 1].A.Id == manifolds[index].A.Id &&
								manifolds[index - 1].B.Id < manifolds[index].B.Id);
		CHECK(ascending);
	}
	for (const ContactManifold &manifold : manifolds) {
		CHECK(manifold.A.Id < manifold.B.Id);
	}
}

TEST_CASE("the manifold list is cleared and not freed", "[physics][narrowphase]") {
	Store store("narrowphase.capacity");
	PreparePhysicsWorld(store, 1.0f);

	for (int index = 0; index < 8; index++) {
		Place(
			store,
			Vector3{static_cast<float>(index) * 0.6f, 0.0f, 0.0f},
			Shaped(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f}),
			true
		);
	}
	Step(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const size_t capacity = PipelineInternals::Manifolds(world).capacity();
	REQUIRE(capacity > 0);

	store.Each<Transform>([](Entity, Transform &transform) {
		transform.Frame.Position = transform.Frame.Position * 100.0f;
	});
	store.MarkAllChanged<Transform>();
	Step(store);

	CHECK(world.Manifolds().empty());
	CHECK(PipelineInternals::Manifolds(world).capacity() == capacity);
}
