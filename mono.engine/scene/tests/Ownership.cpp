// Who simulates a body, and the one case that has a correct answer today.
//
// **Nothing reads ownership yet, so most of what could be asserted here would
// be asserting a plan.** What is real is the shape — absent means the server, a
// null hands it back, a non-`Player` is refused — and the reclaim, which is the
// half that would be a bug rather than a gap the day the rest lands: a body
// owned by a player who left is owned by a dead entity, and an owner nothing can
// resolve is a body nothing will ever simulate.
//
// The reclaim is therefore tested against a *destroyed* player rather than a
// stale handle. Those are different failures — `Entity` carries a generation, so
// a stale handle is already safe — and it is the first one that happens when
// somebody disconnects.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.ownership")
TEST_DEPENDS("engine.scene.services")
TEST_DEPENDS("engine.scene.part")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AddPlayer;
using engine::scene::InstallServices;
using engine::scene::MakePart;
using engine::scene::NetworkOwner;
using engine::scene::NetworkOwnerOf;
using engine::scene::PartDesc;
using engine::scene::ReclaimAbandonedOwnership;
using engine::scene::SetNetworkOwner;

namespace ownership_test {
	void Ready() {
		engine::scene::RegisterSceneClasses();
		engine::scene::RegisterSceneComponents();
	}
}

TEST_CASE("an unassigned body is the server's", "[scene][ownership]") {
	ownership_test::Ready();

	Store store("ownership.default");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});

	// **Both halves, because they are two claims.** The component is absent —
	// which is what keeps ownership free for a game that never uses it — and
	// the accessor reads that absence as the server rather than as an error.
	CHECK_FALSE(store.Has<NetworkOwner>(part));
	CHECK(NetworkOwnerOf(store, part) == NULL_ENTITY);
}

TEST_CASE("a body handed to a player names that player", "[scene][ownership]") {
	ownership_test::Ready();

	Store store("ownership.assign");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});
	const Entity player = AddPlayer(store, "Ada");
	REQUIRE(player != NULL_ENTITY);

	REQUIRE(SetNetworkOwner(store, part, player));
	CHECK(NetworkOwnerOf(store, part) == player);
}

TEST_CASE("a null owner gives the body back rather than storing a hole", "[scene][ownership]") {
	ownership_test::Ready();

	Store store("ownership.release");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});
	const Entity player = AddPlayer(store, "Ada");
	REQUIRE(SetNetworkOwner(store, part, player));

	REQUIRE(SetNetworkOwner(store, part, NULL_ENTITY));

	// The component is gone, not set to a null entity. Absent and "owned by
	// nobody" being one state is what stops a query having to check both.
	CHECK_FALSE(store.Has<NetworkOwner>(part));
	CHECK(NetworkOwnerOf(store, part) == NULL_ENTITY);
}

TEST_CASE("an anchored part is the server's and cannot be handed over", "[scene][ownership]") {
	// **Ownership is about physics, so a part with no physics has nothing to
	// hand over.** Anchoring removes the `RigidBody` — anchored decides
	// presence rather than setting a flag — and an owner of a body that does
	// not exist is a field that reads as live and moves nothing.
	ownership_test::Ready();

	Store store("ownership.anchored");
	InstallServices(store);

	PartDesc anchored;
	anchored.Anchored = true;
	const Entity fixed = MakePart(store, anchored);
	REQUIRE(store.Get<engine::scene::RigidBody>(fixed) == nullptr);

	const Entity player = AddPlayer(store, "Ada");

	CHECK_FALSE(SetNetworkOwner(store, fixed, player));
	CHECK(NetworkOwnerOf(store, fixed) == NULL_ENTITY);

	// Giving it back is still legal, because that is always a legal thing to
	// ask for — a script that anchored a part and then tidied up should not be
	// told off for the order it did them in.
	CHECK(SetNetworkOwner(store, fixed, NULL_ENTITY));
}

TEST_CASE("anchoring an owned body returns it to the server", "[scene][ownership]") {
	// The other end of the same sentence. A script that anchors a part it
	// handed out does not have to remember to take it back — and leaving the
	// row on would keep a client authorised to write the transform of something
	// the world has just declared immovable.
	ownership_test::Ready();

	Store store("ownership.reanchored");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});
	const Entity player = AddPlayer(store, "Ada");
	REQUIRE(SetNetworkOwner(store, part, player));

	// Anchoring, as the property does it: the body goes.
	store.Remove<engine::scene::RigidBody>(part);
	store.Remove<engine::scene::Motion>(part);

	ReclaimAbandonedOwnership(store);
	CHECK(NetworkOwnerOf(store, part) == NULL_ENTITY);
}

TEST_CASE("a non-player is refused and writes nothing", "[scene][ownership]") {
	ownership_test::Ready();

	Store store("ownership.refuse");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});
	const Entity other = MakePart(store, PartDesc{});

	// **Refused rather than stored, because the reclaim below cannot save it.**
	// That only fires for an owner that was alive and stopped being; a body
	// handed to something that was never a player would carry an owner nothing
	// could resolve for the rest of the session.
	CHECK_FALSE(SetNetworkOwner(store, part, other));
	CHECK_FALSE(store.Has<NetworkOwner>(part));
}

TEST_CASE("a body comes back to the server when its owner leaves", "[scene][ownership]") {
	ownership_test::Ready();

	Store store("ownership.reclaim");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});
	const Entity player = AddPlayer(store, "Ada");
	REQUIRE(SetNetworkOwner(store, part, player));

	// Nothing to reclaim while the player is here — which is the half that
	// says the walk is looking at liveness rather than at anything else.
	ReclaimAbandonedOwnership(store);
	CHECK(NetworkOwnerOf(store, part) == player);

	store.DestroyInstance(player);
	REQUIRE_FALSE(store.Alive(player));

	// **The row survives the disconnect and is cleared by the system**, not by
	// the destroy. A player leaving is not told what it owned, so the sweep is
	// what closes it, and the ordering here is exactly the ordering a tick has:
	// the owner goes, and the next `PreSimulation` reclaims.
	CHECK(store.Has<NetworkOwner>(part));
	ReclaimAbandonedOwnership(store);

	CHECK_FALSE(store.Has<NetworkOwner>(part));
	CHECK(NetworkOwnerOf(store, part) == NULL_ENTITY);
}

TEST_CASE("the reclaim runs before anything simulates", "[scene][ownership]") {
	ownership_test::Ready();

	engine::ecs::Scheduler scheduler;
	engine::scene::RegisterOwnershipSystem(scheduler);

	Store store("ownership.scheduled");
	InstallServices(store);

	const Entity part = MakePart(store, PartDesc{});
	const Entity player = AddPlayer(store, "Ada");
	REQUIRE(SetNetworkOwner(store, part, player));

	store.DestroyInstance(player);

	// Through the scheduler rather than by calling the body, because the phase
	// is the claim: a body reclaimed in `PostSimulation` would have been
	// simulated once by nobody first.
	scheduler.RunPhases(store, engine::ecs::Phase::PreSimulation, engine::ecs::Phase::PreSimulation);
	CHECK(NetworkOwnerOf(store, part) == NULL_ENTITY);
}
