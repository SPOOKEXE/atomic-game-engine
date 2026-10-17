#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/ChildWaiters.hpp>
#include <engine/script/InstanceShim.hpp>
#include <engine/script/Instances.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

TEST_SUITE_ID("engine.script.instanceshim")
TEST_DEPENDS("engine.scene.part")

using engine::ecs::Classes;
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
using engine::script::LocalScriptClass;
using engine::script::ModuleScriptClass;
using engine::script::ReadInstanceProperty;
using engine::script::ScriptableProperties;
using engine::script::ScriptableProperty;
using engine::script::ScriptClass;
using engine::script::WriteInstanceProperty;

TEST_CASE("built-in script properties have inspector groups", "[script][instance-shim][properties]") {
	for (const engine::ecs::ClassId owner : {ScriptClass(), LocalScriptClass(), ModuleScriptClass()}) {
		REQUIRE(owner.IsValid());
		for (const auto &property : Classes::Describe(owner).Properties) {
			INFO(property.Spelling);
			CHECK(property.PropertiesTag.IsValid());
		}
	}
}

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

TEST_CASE("the instance shim preserves event endpoints from a port", "[script][instance-shim]") {
	engine::script::ScriptClass();
	Store store("instance_shim_events");

	for (const std::string_view name : {"RemoteEvent", "BindableEvent"}) {
		const auto event = CreateScriptInstance(store, name);
		REQUIRE(event);
		CHECK(InstanceIsA(store, event.Instance, engine::ecs::Classes::Find(engine::core::Name("Instance"))));
	}
}

TEST_CASE(
	"the instance shim reports creation failures without leaving an orphan", "[script][instance-shim]"
) {
	engine::scene::EnsureClassTree();
	Store store("instance_shim_test");

	const auto unknown = CreateScriptInstance(store, "NotAClass");
	CHECK_FALSE(unknown);
	CHECK(unknown.Failure == InstanceCreateFailure::UnknownClass);

	const engine::ecs::ClassId virtualClass = engine::ecs::Classes::Register(
		"VirtualInstanceShim", engine::ecs::Classes::Find(engine::core::Name("Instance")), {}
	);
	engine::ecs::Classes::SetCreatable(virtualClass, false);
	const auto notCreatable = CreateScriptInstance(store, "VirtualInstanceShim");
	CHECK_FALSE(notCreatable);
	CHECK(notCreatable.Failure == InstanceCreateFailure::NotCreatable);

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
	const auto local = CreateScriptInstance(store, "Part");
	REQUIRE(local);
	CHECK(Store::IsPredicted(local.Instance));
	CHECK(store.Has<engine::ecs::ClientLocal>(local.Instance));
}

TEST_CASE("the instance shim is the only scriptable property door", "[script][instance-shim]") {
	engine::scene::EnsureClassTree();
	Store store("instance_shim_test");

	const auto part = CreateScriptInstance(store, "Part");
	REQUIRE(part);
	const auto *canCollide = ScriptableProperty(store, part.Instance, "CanCollide");
	REQUIRE(canCollide != nullptr);
	const auto *canQuery = ScriptableProperty(store, part.Instance, "CanQuery");
	REQUIRE(canQuery != nullptr);

	const bool written = false;
	REQUIRE(WriteInstanceProperty(store, part.Instance, *canCollide, &written, sizeof(written)));
	bool read = true;
	REQUIRE(ReadInstanceProperty(store, part.Instance, *canCollide, &read, sizeof(read)));
	CHECK(read == written);
	REQUIRE(WriteInstanceProperty(store, part.Instance, *canQuery, &written, sizeof(written)));
	REQUIRE(ReadInstanceProperty(store, part.Instance, *canQuery, &read, sizeof(read)));
	CHECK(read == written);

	engine::script::ScriptClass();
	const auto sourceBase = engine::ecs::Classes::Find(engine::core::Name("LuaSourceContainer"));
	REQUIRE(sourceBase.IsValid());
	CHECK_FALSE(engine::ecs::Classes::Describe(sourceBase).Creatable);
	CHECK(engine::ecs::Classes::Describe(engine::ecs::Classes::Find(engine::core::Name("Script"))).Creatable);
	const auto script = CreateScriptInstance(store, "Script");
	REQUIRE(script);
	CHECK(ScriptableProperty(store, script.Instance, "LuaSource") == nullptr);

	const auto visible = ScriptableProperties(store, script.Instance);
	CHECK(std::none_of(visible.begin(), visible.end(), [](const auto *property) {
		return property->Spelling == "LuaSource" || property->Spelling == "JavaScriptSource";
	}));
}

TEST_CASE(
	"client instance fields stay local while authority and resources stay owned", "[script][instance-shim]"
) {
	engine::scene::EnsureClassTree();
	Store store("instance_shim_ownership");
	const auto authority = CreateScriptInstance(store, "Part");
	const auto local = CreateScriptInstance(store, "Part", engine::ecs::NULL_ENTITY, true);
	REQUIRE(authority);
	REQUIRE(local);
	const bool value = false;
	const auto *field = ScriptableProperty(store, local.Instance, "CanCollide");
	REQUIRE(field != nullptr);
	CHECK(WriteInstanceProperty(store, local.Instance, *field, &value, sizeof(value), true));
	CHECK_FALSE(WriteInstanceProperty(store, local.Instance, *field, &value, sizeof(value), false));
	CHECK_FALSE(engine::script::InstanceVisibleToScript(store, local.Instance, false));
	CHECK(engine::script::InstanceVisibleToScript(store, local.Instance, true));
	CHECK(engine::script::CloneScriptInstance(store, local.Instance, false) == engine::ecs::NULL_ENTITY);
	CHECK_FALSE(CreateScriptInstance(store, "Camera", engine::ecs::NULL_ENTITY, false));

	CHECK_FALSE(WriteInstanceProperty(store, authority.Instance, *field, &value, sizeof(value), true));
	store.SetAdoptOnly(true);
	CHECK(WriteInstanceProperty(store, local.Instance, *field, &value, sizeof(value), true));
	CHECK_FALSE(WriteInstanceProperty(store, authority.Instance, *field, &value, sizeof(value), true));
	const auto copy = engine::script::CloneScriptInstance(store, authority.Instance, true);
	REQUIRE(copy != engine::ecs::NULL_ENTITY);
	CHECK(Store::IsPredicted(copy));
	CHECK(store.Has<engine::ecs::ClientLocal>(copy));
	const auto camera = CreateScriptInstance(store, "Camera");
	REQUIRE(camera);
	CHECK(Store::IsPredicted(camera.Instance));
	CHECK(engine::ecs::Classes::Describe(store.ClassOf(camera.Instance)).RuntimeLocal);
	CHECK_FALSE(
		engine::ecs::Classes::Describe(engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")))
			.RuntimeLocal
	);
}

TEST_CASE("GUI tree reads isolate server sources from local copies", "[script][instance-shim][playergui]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	Store store("instance_shim_gui_views");
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "Ada", true);
	const auto container = store.FindFirstChild(player, "PlayerGui");
	const auto source = store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
	REQUIRE(store.SetParent(source, container));
	const auto label = store.CreateInstance(gui::GuiClass("TextLabel"), "Status");
	REQUIRE(store.SetParent(label, source));
	store.GetMutable<gui::Label>(label)->Text = "server";
	REQUIRE(gui::RefreshPlayerGuiProjection(store, player, {}) == 1);
	const auto copy = gui::FindPlayerGuiCopy(store, source);
	const auto localLabel = gui::FindPlayerGuiCopy(store, label);
	REQUIRE(store.Alive(copy));
	REQUIRE(store.Alive(localLabel));
	REQUIRE(store.SetInstanceName(copy, "LocalHud"));
	store.GetMutable<gui::Label>(localLabel)->Text = "client";

	std::vector<Entity> serverChildren, clientChildren;
	script::EachInstanceChild(store, container, [&](Entity entity) { serverChildren.push_back(entity); });
	script::EachInstanceChild(
		store, container, [&](Entity entity) { clientChildren.push_back(entity); }, true
	);
	CHECK(serverChildren == std::vector<Entity>{source});
	CHECK(clientChildren == std::vector<Entity>{copy});
	CHECK(FindInstanceChild(store, container, "Hud") == source);
	CHECK(FindInstanceChild(store, container, "Hud", false, true) == ecs::NULL_ENTITY);
	CHECK(FindInstanceChild(store, container, "LocalHud", false, true) == copy);
	CHECK(FindInstanceChild(store, source, "Status", false, true) == localLabel);
	CHECK(FindInstanceChild(store, container, "Status", true, true) == localLabel);
	CHECK(FindInstanceChild(store, container, "Status", true, false) == label);
	CHECK(script::InstanceForScriptRead(store, source, true) == copy);
	CHECK(script::InstanceForScriptRead(store, copy, false) == ecs::NULL_ENTITY);
	std::vector<Entity> descendants;
	script::EachInstanceDescendant(
		store, container, [&](Entity entity) { descendants.push_back(entity); }, true
	);
	CHECK(descendants == std::vector<Entity>{copy, localLabel});

	const auto *text = ScriptableProperty(store, label, "Text");
	REQUIRE(text != nullptr);
	std::string read;
	REQUIRE(ReadInstanceProperty(store, label, *text, &read, sizeof(read), true));
	CHECK(read == "client");
	REQUIRE(ReadInstanceProperty(store, label, *text, &read, sizeof(read), false));
	CHECK(read == "server");
	CHECK_FALSE(ReadInstanceProperty(store, localLabel, *text, &read, sizeof(read), false));
	const auto *parent = ScriptableProperty(store, label, "Parent");
	REQUIRE(parent != nullptr);
	Entity parentRead;
	REQUIRE(ReadInstanceProperty(store, label, *parent, &parentRead, sizeof(parentRead), true));
	CHECK(parentRead == copy);

	const auto local = script::CreateScriptInstance(store, "ScreenGui", container, true);
	REQUIRE(local);
	CHECK_FALSE(script::InstanceVisibleToScript(store, local.Instance, false));
	CHECK(script::InstanceVisibleToScript(store, local.Instance, true));
	store.DestroyInstance(copy);
	CHECK(script::InstanceForScriptRead(store, source, true) == ecs::NULL_ENTITY);
	CHECK(FindInstanceChild(store, container, "Hud", false, true) == ecs::NULL_ENTITY);
	CHECK(store.Alive(source));
}

TEST_CASE("shared host child waits retain their originating GUI view", "[script][instance-shim][playergui]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	Store store("instance_shim_gui_waits");
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "Ada", true);
	const auto container = store.FindFirstChild(player, "PlayerGui");
	script::ChildWaiters waits;
	const auto server = waits.Add(container, "Hud", 10, false);
	const auto client = waits.Add(container, "Hud", 10, true);
	std::vector<script::ChildWaiters::Resumption> ready;
	waits.Advance(store, 1, ready);
	CHECK(ready.empty());
	const auto source = store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
	REQUIRE(store.SetParent(source, container));
	REQUIRE(gui::RefreshPlayerGuiProjection(store, player, {}) == 1);
	const auto copy = gui::FindPlayerGuiCopy(store, source);
	REQUIRE(store.Alive(copy));
	waits.Advance(store, 2, ready);
	REQUIRE(ready.size() == 2);
	CHECK(ready[0].Waiter == server);
	CHECK(ready[0].Child == source);
	CHECK(ready[1].Waiter == client);
	CHECK(ready[1].Child == copy);
	CHECK(waits.Empty());
}
