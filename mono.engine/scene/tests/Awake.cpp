// Keeping a world ticking when nobody is in it.
//
// **The point of this component is that a host cannot work the answer out.** A
// server sees players and an editor sees players and viewports; neither can see
// that an NPC is halfway along a route or that a round timer has forty seconds
// left. So what is asserted here is the shape of the claim rather than any
// policy - the policy is `world::DecideLifecycle` and has its own suite.
//
// The case worth having is the last one: a claim dies with the entity that made
// it. That is the whole reason this is a component rather than a flag on the
// world, and it is the half that would be a leak rather than a gap.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.awake")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AwakeWorld;
using engine::scene::HoldsWorldAwake;
using engine::scene::KeepWorldAwake;
using engine::scene::LetWorldSleep;
using engine::scene::MakePart;
using engine::scene::PartDesc;
using engine::scene::WorldIsHeldAwake;

namespace awake_test {
	void Ready() {
		engine::scene::RegisterSceneClasses();
		engine::scene::RegisterSceneComponents();
	}
}

TEST_CASE("a world with no claim is not held awake", "[scene][awake]") {
	awake_test::Ready();

	Store store("awake.none");
	engine::scene::InstallServices(store);
	(void)MakePart(store, PartDesc{});

	// The shape every game that never uses this has, and it costs a walk over an
	// archetype with nothing in it.
	CHECK_FALSE(WorldIsHeldAwake(store));
}

TEST_CASE("a claim holds the world awake and names why", "[scene][awake]") {
	awake_test::Ready();

	Store store("awake.claim");
	engine::scene::InstallServices(store);

	const Entity npc = MakePart(store, PartDesc{});
	REQUIRE(KeepWorldAwake(store, npc, Name("patrol")));

	CHECK(HoldsWorldAwake(store, npc));

	// **The reason comes back**, which is the whole reason it is required. The
	// question somebody asks about a world that will not sleep is not whether
	// something is holding it up but what.
	Name reason;
	CHECK(WorldIsHeldAwake(store, &reason));
	CHECK(reason == Name("patrol"));
}

TEST_CASE("restating a claim replaces it rather than stacking", "[scene][awake]") {
	awake_test::Ready();

	Store store("awake.restate");
	engine::scene::InstallServices(store);

	const Entity npc = MakePart(store, PartDesc{});
	REQUIRE(KeepWorldAwake(store, npc, Name("patrol")));
	REQUIRE(KeepWorldAwake(store, npc, Name("returning")));

	// A caller may say this every tick without accumulating anything, which is
	// what makes it usable from a script that does not track its own state.
	Name reason;
	REQUIRE(WorldIsHeldAwake(store, &reason));
	CHECK(reason == Name("returning"));

	LetWorldSleep(store, npc);
	CHECK_FALSE(WorldIsHeldAwake(store));
}

TEST_CASE("withdrawing a claim nobody made is not an error", "[scene][awake]") {
	awake_test::Ready();

	Store store("awake.tidy");
	engine::scene::InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});

	// A script tidying up should not have to remember whether it made the claim
	// - the same argument `SetNetworkOwner` makes for handing a body back.
	LetWorldSleep(store, part);
	CHECK_FALSE(WorldIsHeldAwake(store));
}

TEST_CASE("a dead entity cannot claim", "[scene][awake]") {
	awake_test::Ready();

	Store store("awake.dead");
	engine::scene::InstallServices(store);

	CHECK_FALSE(KeepWorldAwake(store, NULL_ENTITY, Name("nothing")));
	CHECK_FALSE(WorldIsHeldAwake(store));
}

TEST_CASE("one of several claims is enough, and the last one going releases it", "[scene][awake]") {
	awake_test::Ready();

	Store store("awake.several");
	engine::scene::InstallServices(store);

	const Entity first = MakePart(store, PartDesc{});
	const Entity second = MakePart(store, PartDesc{});
	REQUIRE(KeepWorldAwake(store, first, Name("patrol")));
	REQUIRE(KeepWorldAwake(store, second, Name("restock")));

	LetWorldSleep(store, first);

	// **Still held**, because the question is whether any entity needs the
	// world rather than whether a particular one does.
	CHECK(WorldIsHeldAwake(store));

	LetWorldSleep(store, second);
	CHECK_FALSE(WorldIsHeldAwake(store));
}

TEST_CASE("a claim dies with the entity that made it", "[scene][awake]") {
	// **The case this is a component for.** A flag on the world outlives
	// whatever set it: somebody marks a world awake for an NPC, the NPC is
	// destroyed, and the code that would have cleared the flag is exactly the
	// code that no longer runs - leaving a world that never sleeps for a reason
	// nobody can find. Attaching the claim to the thing that needs it makes the
	// release automatic and unforgettable.
	awake_test::Ready();

	Store store("awake.destroyed");
	engine::scene::InstallServices(store);

	const Entity npc = MakePart(store, PartDesc{});
	REQUIRE(KeepWorldAwake(store, npc, Name("patrol")));
	REQUIRE(WorldIsHeldAwake(store));

	store.DestroyInstance(npc);

	CHECK_FALSE(store.Alive(npc));
	CHECK_FALSE(WorldIsHeldAwake(store));
}
