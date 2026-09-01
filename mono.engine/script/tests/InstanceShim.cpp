#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/InstanceShim.hpp>
#include <engine/script/Instances.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

TEST_SUITE_ID("engine.script.instanceshim")
TEST_DEPENDS("engine.scene.part")

using engine::ecs::Entity;
using engine::ecs::Store;
using engine::script::CreateScriptInstance;
using engine::script::FindInstanceChild;
using engine::script::InstanceAlive;
using engine::script::InstanceClassOf;
using engine::script::InstanceCreateFailure;
using engine::script::InstanceIsA;
using engine::script::InstanceNameOf;
using engine::script::InstanceParentOf;
using engine::script::ReadInstanceProperty;
using engine::script::ScriptableProperties;
using engine::script::ScriptableProperty;
using engine::script::WriteInstanceProperty;

TEST_CASE("the instance shim creates and parents ECS instances", "[script][instance-shim]") {
	engine::scene::EnsureClassTree();
	Store store("instance_shim_test");

	const auto root = CreateScriptInstance(store, "Part");
	REQUIRE(root);
	const auto child = CreateScriptInstance(store, "Part", root.Instance);
	REQUIRE(child);

	CHECK(store.IsA(root.Instance, engine::scene::PartClass()));
	CHECK(InstanceAlive(store, root.Instance));
	CHECK(InstanceClassOf(store, root.Instance) == engine::scene::PartClass());
	CHECK(InstanceIsA(store, child.Instance, engine::scene::PartClass()));
	CHECK(InstanceNameOf(store, child.Instance) == engine::core::Name("Part"));
	CHECK(InstanceParentOf(store, child.Instance) == root.Instance);
	CHECK(FindInstanceChild(store, root.Instance, "Part") == child.Instance);
}

TEST_CASE(
	"the instance shim reports creation failures without leaving an orphan", "[script][instance-shim]"
) {
	engine::scene::EnsureClassTree();
	Store store("instance_shim_test");

	const auto unknown = CreateScriptInstance(store, "NotAClass");
	CHECK_FALSE(unknown);
	CHECK(unknown.Failure == InstanceCreateFailure::UnknownClass);

	const Entity staleParent = store.CreateInstance(engine::scene::PartClass(), "stale");
	REQUIRE(staleParent != engine::ecs::NULL_ENTITY);
	store.DestroyInstance(staleParent);
	const auto refusedParent = CreateScriptInstance(store, "Part", staleParent);
	CHECK_FALSE(refusedParent);
	CHECK(refusedParent.Failure == InstanceCreateFailure::ParentRefused);

	int parts = 0;
	store.Each<const engine::scene::Transform>([&](Entity, const engine::scene::Transform &) { ++parts; });
	CHECK(parts == 0);

	store.SetAdoptOnly(true);
	const auto refusedStore = CreateScriptInstance(store, "Part");
	CHECK_FALSE(refusedStore);
	CHECK(refusedStore.Failure == InstanceCreateFailure::StoreRefused);
}

TEST_CASE("the instance shim is the only scriptable property door", "[script][instance-shim]") {
	engine::scene::EnsureClassTree();
	Store store("instance_shim_test");

	const auto part = CreateScriptInstance(store, "Part");
	REQUIRE(part);
	const auto *canCollide = ScriptableProperty(store, part.Instance, "CanCollide");
	REQUIRE(canCollide != nullptr);

	const bool written = false;
	REQUIRE(WriteInstanceProperty(store, part.Instance, *canCollide, &written, sizeof(written)));
	bool read = true;
	REQUIRE(ReadInstanceProperty(store, part.Instance, *canCollide, &read, sizeof(read)));
	CHECK(read == written);

	engine::script::ScriptClass();
	const auto script = CreateScriptInstance(store, "Script");
	REQUIRE(script);
	CHECK(ScriptableProperty(store, script.Instance, "LuaSource") == nullptr);

	const auto visible = ScriptableProperties(store, script.Instance);
	CHECK(std::none_of(visible.begin(), visible.end(), [](const auto *property) {
		return property->Spelling == "LuaSource" || property->Spelling == "JavaScriptSource";
	}));
}
