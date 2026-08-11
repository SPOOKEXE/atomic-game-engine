#include <engine/core/Name.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

// Private: the registry-level invariant this suite pins — that a query against
// an unprepared world registers nothing — is a property of the lookup in
// `src/WorldResource.hpp`, and there is no public way to ask it.
#include "WorldResource.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

TEST_SUITE_ID("engine.physics.query")
// The indexes these walk, and the fact that a candidate is a box and not a
// shape.
TEST_DEPENDS("engine.physics.broadphase")
// The box-level queries this layer calls for its candidates, and the
// `QueryResult` shape it reuses rather than reinventing.
TEST_DEPENDS("engine.spatial.query")
// The exact tests are against these shapes, read the way `Shapes.hpp` says.
TEST_DEPENDS("engine.physics.shapes")
// Ray and RayHit, whose conventions these answers copy exactly.
TEST_DEPENDS("engine.core.types.ray")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Ray;
using engine::core::Vector3;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::ColliderHit;
using engine::physics::OverlapBox;
using engine::physics::OverlapSphere;
using engine::physics::PhysicsWorld;
using engine::physics::PhysicsWorldRegistered;
using engine::physics::PreparePhysicsWorld;
using engine::physics::Raycast;
using engine::physics::ShapeCast;
using engine::physics::ShapeWorldBounds;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::ShapeKind;
using engine::scene::Transform;
using engine::spatial::LayerMask;
using engine::spatial::QueryResult;

namespace {
	constexpr float EIGHTH_TURN = 0.7853981634f;

	struct Placed {
		Vector3 Position{};
		Vector3 Extent{0.5f, 0.5f, 0.5f};
		ShapeKind Shape = ShapeKind::Box;
		CFrame Rotation{};
		bool Moving = true;
		LayerMask Layer = LayerMask::Only(0);
	};

	Entity Place(Store &store, const Placed &placed) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{placed.Position, placed.Rotation.Rotation()}});

		Collider collider;
		collider.Shape = placed.Shape;
		collider.Extent = placed.Extent;
		collider.Layer = placed.Layer;
		store.Set<Collider>(entity, collider);

		if (placed.Moving) {
			store.Set<Motion>(entity, Motion{});
		}
		return entity;
	}

	void Index(Store &store) {
		SyncBroadphase(store);
		BroadPhase(store);
	}

	Collider Shaped(ShapeKind shape, const Vector3 &extent) {
		Collider collider;
		collider.Shape = shape;
		collider.Extent = extent;
		return collider;
	}
}

// --- raycast ------------------------------------------------------------------

TEST_CASE("a raycast against a rotated box uses the box's own axes", "[physics][query]") {
	// **The case `v02v03v04.md` §3.7 names, and the reason it names it.** A
	// wrong inverse transform passes every axis-aligned test there is, so the
	// box here is long on one axis and turned 45 degrees: an answer taken
	// against the collider's bound arrives at 3.23 metres with a normal of
	// +X, and the right one arrives at 4.29 with a normal 45 degrees off it.
	// The two are not close enough to blame on tolerance.
	Store store("query.rotated");
	PreparePhysicsWorld(store, 4.0f);

	const Entity slab = Place(
		store,
		Placed{
			.Extent = Vector3{2.0f, 0.5f, 0.5f},
			.Rotation = CFrame::Angles(0.0f, EIGHTH_TURN, 0.0f),
		}
	);
	Index(store);

	const std::optional<ColliderHit> hit =
		Raycast(store, Ray{Vector3{5.0f, 0.0f, 0.0f}, -Vector3::XAxis}, 10.0f);

	REQUIRE(hit.has_value());
	CHECK(hit->Owner == slab);

	const float root = std::sqrt(0.5f);
	CHECK(hit->Distance == Approx(5.0f - 0.5f / root).margin(1e-3));
	CHECK(hit->Normal.X == Approx(root).margin(1e-3));
	CHECK(hit->Normal.Y == Approx(0.0f).margin(1e-3));
	CHECK(hit->Normal.Z == Approx(root).margin(1e-3));

	// And the bound really is where the wrong answer would have come from, so
	// the case is measuring the difference rather than asserting a number.
	const AABB bound = ShapeWorldBounds(*store.Get<Collider>(slab), store.Get<Transform>(slab)->Frame);
	CHECK(bound.Maximum.X == Approx(2.5f * root).margin(1e-3));
	CHECK(hit->Distance > 5.0f - bound.Maximum.X);
}

TEST_CASE("a raycast hits a sphere and a cylinder where they are", "[physics][query]") {
	Store store("query.shapes");
	PreparePhysicsWorld(store, 4.0f);

	const Entity ball = Place(store, Placed{.Extent = Vector3{1.0f, 0.0f, 0.0f}, .Shape = ShapeKind::Sphere});
	const Entity barrel = Place(
		store,
		Placed{
			.Position = Vector3{0.0f, 0.0f, 20.0f},
			.Extent = Vector3{1.0f, 2.0f, 0.0f},
			.Shape = ShapeKind::Cylinder
		}
	);
	Index(store);

	const std::optional<ColliderHit> onBall =
		Raycast(store, Ray{Vector3{5.0f, 0.0f, 0.0f}, -Vector3::XAxis}, 10.0f);
	REQUIRE(onBall.has_value());
	CHECK(onBall->Owner == ball);
	CHECK(onBall->Distance == Approx(4.0f).margin(1e-3));
	CHECK(onBall->Normal.X == Approx(1.0f).margin(1e-3));

	// Down the barrel's axis onto its flat end: the cap, at its half-height.
	const std::optional<ColliderHit> onCap =
		Raycast(store, Ray{Vector3{0.0f, 10.0f, 20.0f}, -Vector3::YAxis}, 20.0f);
	REQUIRE(onCap.has_value());
	CHECK(onCap->Owner == barrel);
	CHECK(onCap->Distance == Approx(8.0f).margin(1e-3));
	CHECK(onCap->Normal.Y == Approx(1.0f).margin(1e-3));

	// And across it onto the wall, at its radius.
	const std::optional<ColliderHit> onWall =
		Raycast(store, Ray{Vector3{6.0f, 0.0f, 20.0f}, -Vector3::XAxis}, 20.0f);
	REQUIRE(onWall.has_value());
	CHECK(onWall->Owner == barrel);
	CHECK(onWall->Distance == Approx(5.0f).margin(1e-3));
	CHECK(onWall->Normal.X == Approx(1.0f).margin(1e-3));
}

TEST_CASE("a raycast misses a shape its bound would have caught", "[physics][query]") {
	// The whole difference between this `Raycast` and `spatial`'s. A ray
	// through the corner of a sphere's bounding box meets the box and not the
	// sphere, and a caller that reached for the wrong function gets an answer
	// that is a box away from right.
	Store store("query.corner");
	PreparePhysicsWorld(store, 4.0f);

	const Entity ball = Place(store, Placed{.Extent = Vector3{1.0f, 0.0f, 0.0f}, .Shape = ShapeKind::Sphere});
	Index(store);

	const Ray corner{Vector3{5.0f, 0.95f, 0.95f}, -Vector3::XAxis};
	CHECK_FALSE(Raycast(store, corner, 10.0f).has_value());

	// The ray really does pass through the collider's bound, which is what
	// makes the miss above a decision rather than an accident of the ray
	// missing everything: 0.95 squared twice is 1.8, well outside a unit
	// sphere and comfortably inside the box around it.
	const AABB bound = ShapeWorldBounds(*store.Get<Collider>(ball), store.Get<Transform>(ball)->Frame);
	CHECK(bound.Contains(Vector3{0.0f, 0.95f, 0.95f}));

	// And a ray straight at the middle of the same sphere does hit, so the
	// collider is in the index and the case is measuring the corner rather
	// than an empty world.
	CHECK(Raycast(store, Ray{Vector3{5.0f, 0.0f, 0.0f}, -Vector3::XAxis}, 10.0f).has_value());
}

TEST_CASE("a raycast keeps the nearest of several colliders", "[physics][query]") {
	Store store("query.nearest");
	PreparePhysicsWorld(store, 2.0f);

	Place(store, Placed{.Position = Vector3{6.0f, 0.0f, 0.0f}});
	const Entity near = Place(store, Placed{.Position = Vector3{2.0f, 0.0f, 0.0f}});
	Place(store, Placed{.Position = Vector3{4.0f, 0.0f, 0.0f}});
	Index(store);

	const std::optional<ColliderHit> hit = Raycast(store, Ray{Vector3::Zero, Vector3::XAxis}, 20.0f);
	REQUIRE(hit.has_value());
	CHECK(hit->Owner == near);
	CHECK(hit->Distance == Approx(1.5f).margin(1e-3));
}

TEST_CASE("a raycast finds both static and moving colliders", "[physics][query]") {
	// Two indexes and one answer. A query that walked only the dynamic one
	// would miss every floor in the world.
	Store store("query.bothindexes");
	PreparePhysicsWorld(store, 2.0f);

	const Entity anchored = Place(store, Placed{.Position = Vector3{3.0f, 0.0f, 0.0f}, .Moving = false});
	Index(store);

	const std::optional<ColliderHit> hit = Raycast(store, Ray{Vector3::Zero, Vector3::XAxis}, 20.0f);
	REQUIRE(hit.has_value());
	CHECK(hit->Owner == anchored);
}

TEST_CASE("a raycast respects the layer mask", "[physics][query]") {
	Store store("query.layers");
	PreparePhysicsWorld(store, 2.0f);

	Place(store, Placed{.Position = Vector3{2.0f, 0.0f, 0.0f}, .Layer = LayerMask::Only(1)});
	const Entity wanted =
		Place(store, Placed{.Position = Vector3{4.0f, 0.0f, 0.0f}, .Layer = LayerMask::Only(2)});
	Index(store);

	const std::optional<ColliderHit> hit =
		Raycast(store, Ray{Vector3::Zero, Vector3::XAxis}, 20.0f, LayerMask::Only(2));
	REQUIRE(hit.has_value());
	CHECK(hit->Owner == wanted);
}

TEST_CASE("a raycast with no direction or no distance finds nothing", "[physics][query]") {
	Store store("query.degenerate");
	PreparePhysicsWorld(store, 2.0f);

	Place(store, Placed{.Position = Vector3{2.0f, 0.0f, 0.0f}});
	Index(store);

	CHECK_FALSE(Raycast(store, Ray{}, 10.0f).has_value());
	CHECK_FALSE(Raycast(store, Ray{Vector3::Zero, Vector3::XAxis}, 0.0f).has_value());
	CHECK_FALSE(Raycast(store, Ray{Vector3::Zero, Vector3::XAxis}, 1.0f).has_value());
}

TEST_CASE("a query against a world with no physics resource finds nothing", "[physics][query]") {
	// Loud in the log and empty in the answer. A query that returned a
	// plausible miss with no complaint would look exactly like an empty world.
	//
	// **And it must register nothing on the way to saying so.** That half is
	// not decoration: `Store::Resource<PhysicsWorld>()` registers the type in
	// order to report that it is missing, under the compiler's spelling, and
	// the next `RegisterPhysicsComponents` then aborts the process because a
	// type may not have two names. It hides because it needs a query to run
	// *before* any registration, which in a shuffled suite is a matter of the
	// seed — it was reported as an unreproducible flake first, and it is not
	// one. `src/WorldResource.hpp` carries the whole account.
	const bool registeredBefore = PhysicsWorldRegistered();

	Store store("query.unprepared");
	std::array<Entity, 4> found{};

	CHECK_FALSE(Raycast(store, Ray{Vector3::Zero, Vector3::XAxis}, 10.0f).has_value());
	CHECK(
		OverlapBox(store, AABB::FromCentre(Vector3::Zero, Vector3::One), LayerMask::All(), found).Written == 0
	);
	CHECK(OverlapSphere(store, Vector3::Zero, 1.0f, LayerMask::All(), found).Written == 0);
	CHECK(
		ShapeCast(
			store,
			Shaped(ShapeKind::Box, Vector3{0.5f, 0.5f, 0.5f}),
			CFrame{},
			Vector3::XAxis,
			LayerMask::All(),
			found
		)
			.Written == 0
	);

	// The invariant, asserted rather than implied by the suite happening to
	// pass: four queries against a store nobody prepared left the registry
	// exactly as they found it.
	CHECK(PhysicsWorldRegistered() == registeredBefore);

	// **This is the line that catches the original bug**, and it is separate
	// from the one above on purpose: an accidental registration takes the
	// *compiler's* spelling, so the explicit name stays unregistered and the
	// check above is still true. Asked through `TypeNameOf` rather than the
	// string it produces here, so the case means the same thing on every
	// compiler.
	CHECK_FALSE(Components::Find(Name(engine::ecs::TypeNameOf<PhysicsWorld>())).IsValid());
}

// --- overlaps -----------------------------------------------------------------

TEST_CASE("an overlap tests the shape and not the bound", "[physics][query]") {
	// A cylinder standing in the corner of its own bounding box is reported by
	// `spatial::OverlapSphere` from a metre away, correctly — it answers about
	// boxes. This one has to say no.
	Store store("query.exactoverlap");
	PreparePhysicsWorld(store, 4.0f);

	Place(store, Placed{.Extent = Vector3{1.0f, 1.0f, 0.0f}, .Shape = ShapeKind::Cylinder});
	Index(store);

	std::array<Entity, 8> found{};

	// Inside the bound's corner and well outside the barrel.
	const QueryResult corner = OverlapSphere(store, Vector3{0.9f, 0.0f, 0.9f}, 0.1f, LayerMask::All(), found);
	CHECK(corner.Written == 0);
	CHECK_FALSE(corner.Overflowed);

	// The same distance from the centre, but along an axis where the barrel
	// really is.
	const QueryResult side = OverlapSphere(store, Vector3{1.05f, 0.0f, 0.0f}, 0.1f, LayerMask::All(), found);
	CHECK(side.Written == 1);
}

TEST_CASE("an overlap box finds what it encloses", "[physics][query]") {
	Store store("query.overlapbox");
	PreparePhysicsWorld(store, 2.0f);

	const Entity inside = Place(store, Placed{.Position = Vector3{1.0f, 0.0f, 0.0f}});
	Place(store, Placed{.Position = Vector3{9.0f, 0.0f, 0.0f}});
	Index(store);

	std::array<Entity, 8> found{};
	const QueryResult result = OverlapBox(
		store, AABB::FromCentre(Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.2f, 0.2f, 0.2f}), LayerMask::All(), found
	);

	REQUIRE(result.Written == 1);
	CHECK(found[0] == inside);
	CHECK_FALSE(result.Overflowed);
}

TEST_CASE("an overlap says when the caller's span ran out", "[physics][query]") {
	// **Reported rather than inferred.** A span filled exactly to its length
	// and a span that ran out look identical from the count alone, and a
	// truncated overlap read as "and nothing more" is a contact that never
	// happens.
	Store store("query.overflow");
	PreparePhysicsWorld(store, 4.0f);

	for (int index = 0; index < 4; index++) {
		Place(store, Placed{.Position = Vector3{static_cast<float>(index) * 0.2f, 0.0f, 0.0f}});
	}
	Index(store);

	std::array<Entity, 2> narrow{};
	const QueryResult result = OverlapBox(
		store, AABB::FromCentre(Vector3::Zero, Vector3{4.0f, 4.0f, 4.0f}), LayerMask::All(), narrow
	);

	CHECK(result.Written == 2);
	CHECK(result.Overflowed);
}

TEST_CASE("an overlap sphere refuses a negative or missing radius", "[physics][query]") {
	Store store("query.badradius");
	PreparePhysicsWorld(store, 2.0f);

	Place(store, Placed{});
	Index(store);

	std::array<Entity, 4> found{};
	CHECK(OverlapSphere(store, Vector3::Zero, -1.0f, LayerMask::All(), found).Written == 0);

	// Zero is a legitimate radius and is a point-in-shape test, not a refusal.
	CHECK(OverlapSphere(store, Vector3::Zero, 0.0f, LayerMask::All(), found).Written == 1);
}

// --- shape cast ---------------------------------------------------------------

TEST_CASE("a shape cast finds what is on the path", "[physics][query]") {
	Store store("query.shapecast");
	PreparePhysicsWorld(store, 2.0f);

	const Entity ahead = Place(store, Placed{.Position = Vector3{5.0f, 0.0f, 0.0f}, .Moving = false});
	Place(store, Placed{.Position = Vector3{0.0f, 9.0f, 0.0f}, .Moving = false});
	Index(store);

	std::array<Entity, 8> found{};
	const QueryResult result = ShapeCast(
		store,
		Shaped(ShapeKind::Sphere, Vector3{0.4f, 0.0f, 0.0f}),
		CFrame{Vector3::Zero},
		Vector3{8.0f, 0.0f, 0.0f},
		LayerMask::All(),
		found
	);

	REQUIRE(result.Written == 1);
	CHECK(found[0] == ahead);
}

TEST_CASE("a shape cast with no motion is an overlap", "[physics][query]") {
	Store store("query.shapecast.still");
	PreparePhysicsWorld(store, 2.0f);

	const Entity here = Place(store, Placed{.Moving = false});
	Place(store, Placed{.Position = Vector3{5.0f, 0.0f, 0.0f}, .Moving = false});
	Index(store);

	std::array<Entity, 8> found{};
	const QueryResult result = ShapeCast(
		store,
		Shaped(ShapeKind::Box, Vector3{0.2f, 0.2f, 0.2f}),
		CFrame{Vector3::Zero},
		Vector3::Zero,
		LayerMask::All(),
		found
	);

	REQUIRE(result.Written == 1);
	CHECK(found[0] == here);
}

TEST_CASE("a shape cast sweeps a rotated shape by its own bound", "[physics][query]") {
	// The conservative half of the contract, stated as a test so nobody reads
	// the sweep as exact. A cylinder lying diagonally has a bound much larger
	// than itself, and the sweep is of the bound — so a collider beside the
	// path, inside that bound, is reported.
	Store store("query.shapecast.conservative");
	PreparePhysicsWorld(store, 4.0f);

	const Entity beside = Place(
		store,
		Placed{.Position = Vector3{0.0f, 1.6f, 0.0f}, .Extent = Vector3{0.2f, 0.2f, 0.2f}, .Moving = false}
	);
	Index(store);

	std::array<Entity, 8> found{};
	const QueryResult result = ShapeCast(
		store,
		Shaped(ShapeKind::Cylinder, Vector3{0.5f, 2.0f, 0.0f}),
		CFrame{Vector3::Zero, CFrame::Angles(0.0f, 0.0f, EIGHTH_TURN).Rotation()},
		Vector3{1.0f, 0.0f, 0.0f},
		LayerMask::All(),
		found
	);

	REQUIRE(result.Written == 1);
	CHECK(found[0] == beside);
}

TEST_CASE("a raycast can look straight through the thing casting it", "[physics][query]") {
	// **The case a character controller is, and it is not an edge case.** A
	// humanoid asks what is under its feet by casting from just *inside* them —
	// a ray that begins exactly on a face is a coin flip about whether it hits
	// it, and the coin lands differently on two machines, which is a desync
	// arriving through a character controller. With a root collider the full
	// height of the character, that origin is inside its own box, so the nearest
	// hit is always itself.
	//
	// **Testing the answer afterwards cannot recover it**, which is the whole
	// point of this parameter and the reason the studio found it as a character
	// resting perfectly still on a plate it could not jump off: `Raycast`
	// returns the nearest hit and the floor was never in the answer.
	Store store("query.ignore");
	PreparePhysicsWorld(store, 4.0f);

	const Entity caster =
		Place(store, Placed{.Position = Vector3{0.0f, 2.5f, 0.0f}, .Extent = Vector3{1.0f, 2.5f, 0.5f}});
	const Entity floor = Place(
		store,
		Placed{.Position = Vector3{0.0f, -2.0f, 0.0f}, .Extent = Vector3{50.0f, 2.0f, 50.0f}, .Moving = false}
	);
	Index(store);

	// From a tenth of a metre above the caster's own sole, straight down.
	const Ray under{Vector3{0.0f, 0.1f, 0.0f}, Vector3{0.0f, -1.0f, 0.0f}};

	const std::optional<ColliderHit> itself = Raycast(store, under, 0.25f);
	REQUIRE(itself.has_value());
	CHECK(itself->Owner == caster);

	const std::optional<ColliderHit> ground = Raycast(store, under, 0.25f, LayerMask::All(), caster);
	REQUIRE(ground.has_value());
	CHECK(ground->Owner == floor);

	// **Ignoring something that is not in the way changes nothing**, and a null
	// entity ignores nobody — which is what makes the parameter safe to default.
	const std::optional<ColliderHit> unaffected = Raycast(store, under, 0.25f, LayerMask::All(), floor);
	REQUIRE(unaffected.has_value());
	CHECK(unaffected->Owner == caster);

	CHECK(Raycast(store, under, 0.25f, LayerMask::All(), Entity{})->Owner == caster);
}
