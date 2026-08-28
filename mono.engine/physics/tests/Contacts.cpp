#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

TEST_SUITE_ID("engine.physics.contacts")
// A manifold names two entities, and the ordering it promises is the entity
// id's.
TEST_DEPENDS("engine.ecs.store")

using engine::core::Vector3;
using engine::ecs::Entity;
using engine::physics::ContactEvent;
using engine::physics::ContactManifold;
using engine::physics::ContactPhase;
using engine::physics::ContactPoint;

// These cases pin the properties the narrow phase and the solver are written
// against. They were here before either existed, because discovering one of
// them wrong afterwards is a rewrite of both rather than an edit to one; they
// stay because the two now have a producer that has to keep them true.

TEST_CASE("a manifold holds several points", "[physics][contacts]") {
	// A single-point manifold cannot hold a resting box still. One point is one
	// constraint, so a box on a floor pivots about it
	// and rocks, and the rocking never damps because every tick is a fresh
	// single constraint. A capacity of one here would compile and would force
	// the solver, the contact cache and the event surface to be rewritten
	// together later.
	STATIC_REQUIRE(ContactManifold::MAXIMUM_POINTS >= 4);

	ContactManifold manifold;
	manifold.Normal = Vector3::YAxis;
	manifold.PointCount = static_cast<uint8_t>(ContactManifold::MAXIMUM_POINTS);
	for (size_t index = 0; index < ContactManifold::MAXIMUM_POINTS; index++) {
		manifold.Points[index] = ContactPoint{Vector3{static_cast<float>(index), 0.0f, 0.0f}, 0.25f, 7};
	}

	CHECK(manifold.PointCount == ContactManifold::MAXIMUM_POINTS);
	CHECK(manifold.Points[ContactManifold::MAXIMUM_POINTS - 1].Penetration == 0.25f);
}

TEST_CASE("a fresh manifold claims no points", "[physics][contacts]") {
	// The default has to be "no contact", not "one contact at the origin with
	// no depth". A solver handed the second pushes two bodies apart along a
	// zero normal, which is a NaN a long way from here.
	const ContactManifold manifold;

	CHECK(manifold.PointCount == 0);
	CHECK_FALSE(manifold.Trigger);
	CHECK(manifold.Normal == Vector3::Zero);
	CHECK(manifold.A == engine::ecs::NULL_ENTITY);
	CHECK(manifold.B == engine::ecs::NULL_ENTITY);
}

TEST_CASE("a manifold and an event name their bodies the same way round", "[physics][contacts]") {
	// Both order by entity id, and `CandidatePair` does too. Three types with
	// the same two fields and three different conventions is how a solver ends
	// up applying an impulse the wrong way along the normal.
	const Entity smaller{1};
	const Entity larger{2};

	ContactManifold manifold;
	manifold.A = smaller;
	manifold.B = larger;

	const ContactEvent event{manifold.A, manifold.B, ContactPhase::Began};

	CHECK(event.A.Id < event.B.Id);
	CHECK(event.A == manifold.A);
	CHECK(event.B == manifold.B);
}

TEST_CASE("contact types stay trivially copyable", "[physics][contacts]") {
	// The lists are cleared and refilled every tick, and a manifold that grew a
	// vector of points would allocate per contact per tick - the exact shape
	// these types refuse. A fixed array is what makes
	// "cleared, not freed" true of the manifold list and not only of the
	// pointer inside it.
	STATIC_REQUIRE(std::is_trivially_copyable_v<ContactPoint>);
	STATIC_REQUIRE(std::is_trivially_copyable_v<ContactManifold>);
	STATIC_REQUIRE(std::is_trivially_copyable_v<ContactEvent>);
}
