// The two halves of "what a frame between two ticks is drawn at", and the one
// case where the answer is "do not blend at all".
//
// `CapturePreviousTransforms` is one line and needs no suite of its own; what
// this file is for is `SnapPortalTransit`, which exists because the blend is
// wrong across a teleport and a replica has no way to know one happened without
// being told. `scene::PortalTransitSeen` carries the whole argument.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.interpolation")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::PortalTransit;
using engine::scene::PortalTransitSeen;
using engine::scene::PreviousTransform;
using engine::scene::Transform;

namespace {
	// A body somewhere, with a previous frame somewhere else - which is what a
	// replica holds on the frame a teleport arrives.
	Entity Body(Store &store, const Vector3 &was, const Vector3 &now) {
		const Entity entity = store.Create();
		store.Set(entity, Transform{CFrame(now)});
		store.Set(entity, PreviousTransform{CFrame(was)});
		return entity;
	}
}

TEST_CASE("a body that has not crossed anything is left alone", "[scene][interpolation]") {
	engine::scene::RegisterSceneComponents();

	Store store("interpolation.quiet");
	const Entity walker = Body(store, Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f});

	// **No `PortalTransit` at all, which is every body in every scene that has
	// no holes in it.** The archetype filter is what makes this free rather than
	// a branch, and the count is what says so.
	CHECK(engine::scene::SnapPortalTransit(store) == 0);
	CHECK(store.Get<PreviousTransform>(walker)->Frame.Position.X == 0.0f);
}

TEST_CASE("a crossing collapses the blend onto where the body is now", "[scene][interpolation]") {
	engine::scene::RegisterSceneComponents();

	Store store("interpolation.crossed");

	// A hundred units apart, which is the shape a portal pair makes: the two
	// rooms are nowhere near each other and the body is in both across one tick.
	const Entity walker = Body(store, Vector3{0.0f, 6.0f, 0.0f}, Vector3{100.0f, 6.0f, 0.0f});
	store.Set(walker, PortalTransit{1, 0.0f});

	REQUIRE(engine::scene::SnapPortalTransit(store) == 1);

	// The blend now spans nothing, so every frame until the next tick draws the
	// body in the room it arrived in rather than somewhere in the fifty units
	// between the panes.
	CHECK(store.Get<PreviousTransform>(walker)->Frame.Position.X == 100.0f);

	// **And it is taken, so a second frame is not a second snap.** The counter
	// is what makes this idempotent across however many frames a tick lasts.
	const PortalTransitSeen *seen = store.Get<PortalTransitSeen>(walker);
	REQUIRE(seen != nullptr);
	CHECK(seen->Serial == 1);
	CHECK(engine::scene::SnapPortalTransit(store) == 0);
}

TEST_CASE("a second crossing snaps again", "[scene][interpolation]") {
	engine::scene::RegisterSceneComponents();

	Store store("interpolation.again");
	const Entity walker = Body(store, Vector3{0.0f, 0.0f, 0.0f}, Vector3{100.0f, 0.0f, 0.0f});
	store.Set(walker, PortalTransit{1, 0.0f});
	REQUIRE(engine::scene::SnapPortalTransit(store) == 1);

	// Back the other way. **A serial rather than a flag is what makes this
	// work**: nobody had to clear anything, and a delta that never arrived
	// would leave the counters further apart rather than losing the crossing.
	store.Set(walker, Transform{CFrame(Vector3{-40.0f, 0.0f, 0.0f})});
	store.Set(walker, PortalTransit{2, 0.0f});

	CHECK(engine::scene::SnapPortalTransit(store) == 1);
	CHECK(store.Get<PreviousTransform>(walker)->Frame.Position.X == -40.0f);
}

TEST_CASE("a viewer that has already seen the crossing does nothing", "[scene][interpolation]") {
	engine::scene::RegisterSceneComponents();

	Store store("interpolation.authority");

	// **This is the authority's state, and it is the case the design turns
	// on.** `CrossPortals` maps `PreviousTransform` through the seam - which
	// keeps the whole tick's motion, expressed in the destination room - and
	// then takes the counter itself. A snap here would throw that away and
	// stand the body still for the rest of the tick, which is the behaviour
	// CodeParade's `prev_pos = pos` has and the one this deliberately does not.
	const Entity walker = Body(store, Vector3{99.0f, 0.0f, 0.0f}, Vector3{100.0f, 0.0f, 0.0f});
	store.Set(walker, PortalTransit{1, 0.0f});
	store.Set(walker, PortalTransitSeen{1});

	CHECK(engine::scene::SnapPortalTransit(store) == 0);
	CHECK(store.Get<PreviousTransform>(walker)->Frame.Position.X == 99.0f);
}
