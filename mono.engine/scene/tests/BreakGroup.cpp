#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/BreakGroup.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.breakgroup")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Motion;
using engine::scene::PartDesc;
using engine::scene::Simulated;

namespace {
	Entity MakeGroup(Store &store) {
		return store.CreateInstance(Classes::Find(Name("BreakGroup")), "Building");
	}

	Entity MakePart(Store &store, Entity parent, bool simulated = false) {
		PartDesc description;
		description.Simulated = simulated;
		const Entity part = engine::scene::MakePart(store, description);
		REQUIRE(store.SetParent(part, parent));
		return part;
	}
}

TEST_CASE("a break group releases every anchored descendant once", "[scene][breakgroup]") {
	engine::scene::RegisterSceneClasses();
	Store store("scene.breakgroup.release");
	const Entity group = MakeGroup(store);
	const Entity first = MakePart(store, group);

	const Entity nested = store.CreateInstance(Classes::Find(Name("Model")), "Floor");
	REQUIRE(store.SetParent(nested, group));
	const Entity second = MakePart(store, nested);
	const Entity moving = MakePart(store, group, true);

	CHECK(engine::scene::ReleaseBreakGroup(store, group) == 2);
	CHECK(store.Has<Simulated>(first));
	CHECK(store.Has<Motion>(first));
	CHECK(store.Has<Simulated>(second));
	CHECK(store.Has<Motion>(second));
	CHECK(store.Has<Simulated>(moving));
	CHECK(store.Has<Motion>(moving));
	CHECK(engine::scene::ReleaseBreakGroup(store, group) == 0);
}

TEST_CASE("only the BreakGroup class can release a subtree", "[scene][breakgroup]") {
	engine::scene::RegisterSceneClasses();
	Store store("scene.breakgroup.class");
	const Entity ordinaryModel = store.CreateInstance(Classes::Find(Name("Model")), "Ordinary");
	const Entity part = MakePart(store, ordinaryModel);

	CHECK(engine::scene::ReleaseBreakGroup(store, ordinaryModel) == 0);
	CHECK_FALSE(store.Has<Simulated>(part));
	CHECK_FALSE(store.Has<Motion>(part));
}
