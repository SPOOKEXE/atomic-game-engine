// Instances outside Workspace carry `OutsideWorld`; instances in it do not.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/WorldMembership.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.worldmembership")

namespace {
	using namespace engine;

	ecs::Entity Storage(const ecs::Store &store) {
		return scene::ServiceOf(store, ecs::Classes::Find(core::Name("ServerStorage")));
	}
}

TEST_CASE("only instances outside Workspace carry the outside marker", "[scene][world-membership]") {
	scene::RegisterSceneClasses();
	ecs::Store store("membership");
	scene::InstallServices(store);
	const ecs::Entity workspace = scene::WorkspaceOf(store);
	const ecs::Entity storage = Storage(store);
	REQUIRE(storage != ecs::NULL_ENTITY);

	const ecs::Entity floor = store.CreateInstance(scene::PartClass(), "Floor");
	REQUIRE(store.SetParent(floor, workspace));
	const ecs::Entity map = store.CreateInstance(scene::ModelClass(), "Map");
	REQUIRE(store.SetParent(map, storage));
	const ecs::Entity wall = store.CreateInstance(scene::PartClass(), "Wall");
	REQUIRE(store.SetParent(wall, map));
	const ecs::Entity loose = store.CreateInstance(scene::PartClass(), "Loose");

	scene::SyncWorldMembership(store);
	CHECK_FALSE(store.Has<scene::OutsideWorld>(workspace));
	CHECK_FALSE(store.Has<scene::OutsideWorld>(floor));
	CHECK(store.Has<scene::OutsideWorld>(storage));
	CHECK(store.Has<scene::OutsideWorld>(map));
	CHECK(store.Has<scene::OutsideWorld>(wall));
	CHECK(store.Has<scene::OutsideWorld>(loose));

	// Moving a model moves everything under it.
	REQUIRE(store.SetParent(map, workspace));
	scene::SyncWorldMembership(store);
	CHECK_FALSE(store.Has<scene::OutsideWorld>(map));
	CHECK_FALSE(store.Has<scene::OutsideWorld>(wall));

	// A move whose change record was cleared before the sync saw it still lands.
	REQUIRE(store.SetParent(map, storage));
	store.ClearChanges();
	scene::SyncWorldMembership(store);
	CHECK(store.Has<scene::OutsideWorld>(map));
	CHECK(store.Has<scene::OutsideWorld>(wall));
	CHECK_FALSE(store.Has<scene::OutsideWorld>(floor));
}

TEST_CASE("a world with no Workspace marks nothing", "[scene][world-membership]") {
	scene::RegisterSceneClasses();
	ecs::Store store("membership.bare");
	const ecs::Entity part = store.CreateInstance(scene::PartClass(), "Part");
	CHECK(scene::SyncWorldMembership(store) == 0);
	CHECK_FALSE(store.Has<scene::OutsideWorld>(part));
}
