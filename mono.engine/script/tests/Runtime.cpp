#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

TEST_SUITE_ID("engine.script.runtime")
// A script's whole vocabulary is the class tree and the property surface, so a
// change to either has to re-run this.
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Collider;
using engine::scene::PartClass;
using engine::scene::RigidBody;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::RuntimeLimits;

namespace {
	// Registers the class tree, which is what `Instance.new` resolves against.
	// A `Store` is not movable, so this is a call rather than a factory.
	void RegisterClasses() {
		engine::scene::EnsureClassTree();
	}

	// The one part a script made, found by walking rather than by being handed
	// a handle — so these tests assert on the world and not on the binding's
	// own bookkeeping.
	Entity OnlyPart(Store &store) {
		Entity found = engine::ecs::NULL_ENTITY;
		store.Each<const Transform, const Bounds>([&](Entity entity, const Transform &, const Bounds &) {
			found = entity;
		});
		return found;
	}
}

TEST_CASE("a script runs and can print", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run("print('hello from luau')"));
	CHECK(runtime->LastError().empty());
}

TEST_CASE("a syntax error is reported rather than thrown", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("this is not luau"));
	CHECK_FALSE(runtime->LastError().empty());
}

TEST_CASE("Instance.new creates a real entity in the world", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run("local part = Instance.new('Part')"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// Not a scripting-only object: it is an instance of the same class the C++
	// path creates, in the same archetype, with the same components.
	CHECK(store.IsA(part, PartClass()));
	CHECK(store.Get<Visual>(part) != nullptr);
}

TEST_CASE("Instance.new refuses a class nobody registered", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("Instance.new('Sausage')"));
	CHECK(runtime->LastError().find("Sausage") != std::string::npos);
}

TEST_CASE("a script writes properties through the descriptor table", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Position = Vector3.new(1, 2, 3)
		part.Size = Vector3.new(4, 4, 4)
		part.Color = Color3.new(0.25, 0.5, 0.75)
		part.Visible = false
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(1.0f));
	CHECK(store.Get<Transform>(part)->Frame.Position.Z == Approx(3.0f));

	// Half of four, because `Size` is a full extent over a stored half. A
	// binding that passed the number straight through would read 4 here.
	CHECK(store.Get<Bounds>(part)->HalfExtent.X == Approx(2.0f));
	CHECK(store.Get<Collider>(part)->Extent.X == Approx(2.0f));

	CHECK(store.Get<Visual>(part)->Tint.G == Approx(0.5f));
	CHECK_FALSE(store.Get<Visual>(part)->Visible);
}

TEST_CASE("a script reads back what it wrote", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Round-tripped inside the VM, so this covers both directions of the
	// marshalling as well as the conversions underneath.
	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Size = Vector3.new(3, 5, 7)
		assert(part.Size.X == 3, 'X')
		assert(part.Size.Y == 5, 'Y')
		assert(part.Size.Z == 7, 'Z')

		part.Position = Vector3.new(-2, 0, 8)
		assert(part.Position.X == -2, 'position')

		part.Visible = false
		assert(part.Visible == false, 'visible')
	)"));
}

TEST_CASE("Orientation is degrees on both sides of the binding", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// The 57x error, asserted from the script side. Radians anywhere in the
	// chain makes this read 0.785 instead of 45.
	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Orientation = Vector3.new(0, 45, 0)
		local back = part.Orientation
		assert(math.abs(back.Y - 45) < 0.01, 'expected 45 degrees, got ' .. tostring(back.Y))
	)"));
}

TEST_CASE("Position keeps the rotation, from the script side", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Orientation = Vector3.new(0, 90, 0)
		part.Position = Vector3.new(5, 0, 0)
		assert(math.abs(part.Orientation.Y - 90) < 0.01, 'rotation was lost')
	)"));
}

TEST_CASE("Anchored moves the entity between archetypes", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Anchored = false
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<RigidBody>(part) != nullptr);

	const auto second = MakeRuntime(store, Language::Luau);
	REQUIRE(second->Run(R"(
		local part = Instance.new('Part')
		part.Anchored = true
		assert(part.Anchored == true, 'anchored')
	)"));
}

TEST_CASE("a property nobody declared is an error, not a silent nil", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// `Reflectance` is the interesting name now: a real Roblox property this
	// engine has no field for. Writing it must say so rather than accept it
	// silently, which is what a plain table would do.
	//
	// It used to be `Transparency`, and that is the point — the gap closed at
	// v0.6, so the test moved to a name that is still a gap rather than
	// asserting something no longer true.
	CHECK_FALSE(runtime->Run("Instance.new('Part').Reflectance = 0.5"));
	CHECK(runtime->LastError().find("Reflectance") != std::string::npos);

	// And the property that arrived is genuinely there, so this suite covers
	// both sides of the same boundary.
	CHECK(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Transparency = 0.5
		assert(math.abs(part.Transparency - 0.5) < 1e-6, 'Transparency did not round-trip')
	)"));
}

TEST_CASE("a value of the wrong type is refused", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Color3 and Vector3 are three floats each. Without the userdata tag this
	// would write a colour into a position and look almost plausible.
	CHECK_FALSE(runtime->Run(R"(
		Instance.new('Part').Position = Color3.new(1, 0, 0)
	)"));
}

TEST_CASE("os and debug are not reachable", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// A wall clock is what makes a run stop replaying. Asserted rather than
	// assumed, because `luaL_openlibs` would have opened both.
	REQUIRE(runtime->Run("assert(os == nil, 'os is reachable')"));
	REQUIRE(runtime->Run("assert(debug == nil, 'debug is reachable')"));
}

TEST_CASE("the globals are frozen", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// One script rewriting the environment the next one runs in is the failure
	// this prevents.
	CHECK_FALSE(runtime->Run("math.floor = function() return 0 end"));
}

TEST_CASE("an endless loop is cut off rather than hanging the world", "[script]") {
	RegisterClasses();
	Store store("script_test");

	RuntimeLimits limits;
	limits.StepBudget = 100000;
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	// Counted rather than timed. A wall-clock deadline would make whether this
	// script finished depend on how busy the machine was, and a recording would
	// then replay differently on a slower one.
	CHECK_FALSE(runtime->Run("while true do end"));
	CHECK(runtime->LastError().find("step budget") != std::string::npos);
}

TEST_CASE("a replica refuses a script's writes", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const Entity part = engine::scene::MakePart(store, engine::scene::PartDesc{});
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	store.SetAdoptOnly(true);

	const auto runtime = MakeRuntime(store, Language::Luau);

	// The authority owns these rows. A script that appeared to write them would
	// present as "my script works sometimes", which is the worst way to learn
	// about replication.
	CHECK_FALSE(runtime->Run(R"(
		local part = Instance.new('Part')
	)"));
}

// --- the second VM ----------------------------------------------------------
//
// The same assertions as the Luau suite above, because the point of two VMs
// over one binding surface is that neither language is the real one. A
// behaviour that held in Luau and not here would mean the surface had drifted
// into two.

TEST_CASE("javascript runs and can print", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	REQUIRE(runtime->Which() == Language::JavaScript);
	REQUIRE(runtime->Run("print('hello from javascript')"));
}

TEST_CASE("javascript reports a syntax error rather than throwing", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	CHECK_FALSE(runtime->Run("function ( {"));
	CHECK_FALSE(runtime->LastError().empty());
}

TEST_CASE("javascript creates the same entities luau does", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	REQUIRE(runtime->Run(R"(
		const part = Instance.new('Part');
		part.Position = Vector3.new(1, 2, 3);
		part.Size = Vector3.new(4, 4, 4);
		part.Color = Color3.new(0.25, 0.5, 0.75);
		part.Visible = false;
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// Same class, same archetype, same components — not a JavaScript-flavoured
	// entity.
	CHECK(store.IsA(part, PartClass()));
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(1.0f));

	// Half of four, because the conversion is the same one. A second binding
	// that passed the number through would read 4 here.
	CHECK(store.Get<Bounds>(part)->HalfExtent.X == Approx(2.0f));
	CHECK(store.Get<Collider>(part)->Extent.X == Approx(2.0f));

	CHECK(store.Get<Visual>(part)->Tint.G == Approx(0.5f));
	CHECK_FALSE(store.Get<Visual>(part)->Visible);
}

TEST_CASE("javascript reads back what it wrote", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	REQUIRE(runtime->Run(R"(
		const part = Instance.new('Part');
		part.Size = Vector3.new(3, 5, 7);
		if (part.Size.X !== 3) throw new Error('X');
		if (part.Size.Y !== 5) throw new Error('Y');

		part.Orientation = Vector3.new(0, 45, 0);
		if (Math.abs(part.Orientation.Y - 45) > 0.01) {
			throw new Error('expected 45 degrees, got ' + part.Orientation.Y);
		}

		part.Position = Vector3.new(5, 0, 0);
		if (Math.abs(part.Orientation.Y - 45) > 0.01) throw new Error('rotation was lost');
	)"));
}

TEST_CASE("javascript refuses an undeclared property", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	// The prototype carries exactly the declared properties, so `Reflectance`
	// is not there to assign to. Silently accepting it — which a plain object
	// would — is what this asserts against.
	CHECK_FALSE(runtime->Run(R"(
		const part = Instance.new('Part');
		part.Reflectance = 0.5;
		if (part.Reflectance !== 0.5) throw new Error('not a real property');
	)"));
}

TEST_CASE("javascript refuses a value of the wrong type", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	// Color3 and Vector3 are three floats each, and the class id is what
	// catches this rather than the shape.
	CHECK_FALSE(runtime->Run("Instance.new('Part').Position = Color3.new(1, 0, 0);"));
}

TEST_CASE("javascript has no wall clock", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	// `Date` is what makes a run stop replaying, and the context is built
	// without it. Asserted rather than assumed, because it is one intrinsic
	// call away from existing.
	REQUIRE(runtime->Run("if (typeof Date !== 'undefined') throw new Error('Date is reachable');"));
}

TEST_CASE("an endless javascript loop is cut off", "[script][js]") {
	RegisterClasses();
	Store store("script_test");

	RuntimeLimits limits;
	limits.StepBudget = 100000;
	const auto runtime = MakeRuntime(store, Language::JavaScript, limits);

	CHECK_FALSE(runtime->Run("while (true) {}"));
}

TEST_CASE("a promise resolves inside the call that started it", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	// **The microtask queue is the host's**, so a reaction runs before `Run`
	// returns rather than at some later point nobody chose. A pending job left
	// outstanding here would be work crossing a tick boundary.
	REQUIRE(runtime->Run(R"(
		let built = null;
		Promise.resolve().then(() => {
			built = Instance.new('Part');
			built.Size = Vector3.new(2, 2, 2);
		});
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Bounds>(part)->HalfExtent.X == Approx(1.0f));
}

TEST_CASE("the language is chosen by extension", "[script][js]") {
	CHECK(engine::script::LanguageOf("scene.luau") == Language::Luau);
	CHECK(engine::script::LanguageOf("scene.lua") == Language::Luau);
	CHECK(engine::script::LanguageOf("/a/b/scene.js") == Language::JavaScript);
	CHECK(engine::script::LanguageOf("scene.ts") == Language::JavaScript);

	// Neither extension. Luau rather than a refusal, because failing a file for
	// its name says nothing about what is in it.
	CHECK(engine::script::LanguageOf("scene") == Language::Luau);
}

// --- RunService.Heartbeat ---------------------------------------------------
//
// The difference between a scene format and a scripting layer. Without a beat,
// the most a script can do is describe a world and hand it to a C++ system to
// animate.

TEST_CASE("a luau script animates through Heartbeat", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		local travelled = 0
		RunService.Heartbeat:Connect(function(delta)
			travelled += delta
			part.Position = Vector3.new(travelled, 0, 0)
		end)
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// Nothing has moved until the world beats, which is the point: connecting
	// registers behaviour, it does not run it.
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(0.0f));

	REQUIRE(runtime->Heartbeat(0.5f));
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(0.5f));

	REQUIRE(runtime->Heartbeat(0.25f));
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(0.75f));
}

TEST_CASE("a javascript script animates through Heartbeat", "[script][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	// The same behaviour, spelled the way JavaScript spells a method call.
	REQUIRE(runtime->Run(R"(
		const part = Instance.new('Part');
		let travelled = 0;
		RunService.Heartbeat.Connect((delta) => {
			travelled += delta;
			part.Position = Vector3.new(travelled, 0, 0);
		});
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(0.0f));

	REQUIRE(runtime->Heartbeat(0.5f));
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(0.5f));
}

TEST_CASE("a beat with no connections is not an error", "[script]") {
	RegisterClasses();
	Store store("script_test");

	// The ordinary case for a scene that only describes. It must not be a
	// failure a host has to special-case.
	CHECK(MakeRuntime(store, Language::Luau)->Heartbeat(0.016f));
	CHECK(MakeRuntime(store, Language::JavaScript)->Heartbeat(0.016f));
}

TEST_CASE("one raising connection does not stop the others", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		RunService.Heartbeat:Connect(function() error('first') end)
		RunService.Heartbeat:Connect(function() part.Size = Vector3.new(6, 6, 6) end)
	)"));

	// The beat reports the failure and still runs what came after it. A script
	// that threw once must not silently stop everything registered later — the
	// symptom, half a scene animating, points nowhere near the cause.
	CHECK_FALSE(runtime->Heartbeat(0.016f));
	CHECK_FALSE(runtime->LastError().empty());

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Bounds>(part)->HalfExtent.X == Approx(3.0f));
}

TEST_CASE("a script-created Part is drawable", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run("Instance.new('Part')"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// `CollectInstances` matches on <Transform, PreviousTransform, Bounds,
	// Visual>. A part missing the previous transform is a complete, correct
	// part that the renderer silently skips — which is what happened before
	// `BasePart` carried it.
	CHECK(store.Get<engine::scene::PreviousTransform>(part) != nullptr);
	CHECK(store.Get<Transform>(part) != nullptr);
	CHECK(store.Get<Bounds>(part) != nullptr);
	CHECK(store.Get<Visual>(part) != nullptr);
}

// --- the Roblox surface -----------------------------------------------------
//
// A script that reads like Roblox needs the idioms Roblox scripts are built
// out of. These are the ones the ring scene uses.

TEST_CASE("CFrame composes, in both languages", "[script]") {
	RegisterClasses();
	Store store("script_test");

	// `a * b` in Luau. Turning a quarter circle about Y and then stepping five
	// units along local X puts the part on the Z axis, not the X axis — which
	// is the whole reason an orbit needs no sine and no cosine.
	const auto luau = MakeRuntime(store, Language::Luau);
	REQUIRE(luau->Run(R"(
		local part = Instance.new('Part')
		part.CFrame = CFrame.Angles(0, math.rad(90), 0) * CFrame.new(5, 0, 0)
	)"));

	Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Transform>(part)->Frame.Position.X == Approx(0.0f).margin(1.0e-3));
	CHECK(store.Get<Transform>(part)->Frame.Position.Z == Approx(-5.0f).margin(1.0e-3));
}

TEST_CASE("CFrame.Angles is radians, and Orientation is degrees", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Roblox's own split, reproduced deliberately. A binding that took degrees
	// in `CFrame.Angles` would make every `math.rad(90)` a rotation 57 times
	// too small — and the scene would look wrong rather than fail.
	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.CFrame = CFrame.Angles(0, math.rad(90), 0)
		assert(math.abs(part.Orientation.Y - 90) < 0.01, 'got ' .. tostring(part.Orientation.Y))
	)"));
}

TEST_CASE("game:GetService reaches RunService", "[script]") {
	RegisterClasses();
	Store store("script_test");

	// The line every Roblox script opens with, and the same object either way
	// round — two objects for one service would be two things to keep in step.
	REQUIRE(MakeRuntime(store, Language::Luau)->Run("assert(game:GetService('RunService') == RunService)"));

	REQUIRE(MakeRuntime(store, Language::JavaScript)
				->Run("if (game.GetService('RunService') !== RunService) throw new Error('x');"));
}

TEST_CASE("a service nobody provides is an error", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Naming it beats returning nil: a script that gets nil back fails one line
	// later, somewhere that says nothing about the cause.
	//
	// `MarketplaceService` rather than `Players`, which this used to name and
	// which the engine now provides. A test whose example becomes real is a test
	// that stops testing what it says.
	CHECK_FALSE(runtime->Run("game:GetService('MarketplaceService')"));
	CHECK(runtime->LastError().find("MarketplaceService") != std::string::npos);
}

TEST_CASE("workspace is an instance in the world", "[script]") {
	RegisterClasses();
	Store store("test.world");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// **The inverse of what this asserted before v0.7, deliberately.**
	// `workspace` used to stand for the world itself, so it had the world's
	// name and `part.Parent = workspace` meant "a root of this world". A world
	// now holds a real `Workspace` service and the two were collapsed into it —
	// see `script/Bindings.hpp` on `OpenWorkspace`. It is an ordinary instance:
	// it has a class, a name of its own, and a place in the tree.
	REQUIRE(runtime->Run(R"(
		assert(typeof(workspace) == 'Instance', typeof(workspace))
		assert(workspace.Name == 'Workspace', workspace.Name)
		assert(workspace:IsA('Workspace'))

		local part = Instance.new('Part')
		part.Parent = workspace

		-- Read back as the same value, not a second object that behaves alike.
		assert(part.Parent == workspace, 'Parent did not round-trip')
		assert(part:IsDescendantOf(workspace), 'not in the scene')
	)"));

	// It is reached by the same name from `game`, and it is the same instance
	// rather than a second handle onto it.
	REQUIRE(runtime->Run("assert(game.Workspace == workspace)"));
	REQUIRE(runtime->Run("assert(game:GetService('Workspace') == workspace)"));

	// The part, the Workspace, and the nine other services `InstallServices`
	// puts in every world. A phantom row standing for the world is exactly what
	// there is still none of — `workspace` names something that was already
	// there.
	//
	// The count moves whenever a service is added, and it is written out rather
	// than derived on purpose: a service arriving without somebody noticing is
	// exactly what this number is here to make impossible.
	size_t instances = 0;
	store.Each<const engine::ecs::InstanceClass>([&](Entity, const engine::ecs::InstanceClass &) {
		instances++;
	});
	CHECK(instances == 11);
}

// **The rule the render gate rests on**, stated from the script side: an
// instance with no parent is not in the scene, and until v0.7 there was no way
// to say that at all — a null parent meant "a root of this world", which was
// drawn.
TEST_CASE("an instance with no parent is an orphan, not a root of the world", "[script]") {
	RegisterClasses();
	Store store("test.world");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')

		-- Nil, where this used to hand back `workspace`.
		assert(part.Parent == nil, 'a new instance is parented to nothing')
		assert(not part:IsDescendantOf(workspace), 'an orphan is not in the scene')

		-- And no walk of the tree reaches it, which is what makes it usable as
		-- data: a template, a marker, a thing a script holds and never shows.
		for _, child in workspace:GetChildren() do
			assert(child ~= part, 'an orphan was listed')
		end

		-- Saying where it goes is what puts it in the scene.
		part.Parent = workspace
		assert(part:IsDescendantOf(workspace))
	)"));
}

TEST_CASE("javascript sees the same world", "[script][js]") {
	RegisterClasses();
	Store store("test.world");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	REQUIRE(runtime->Run(R"(
		if (workspace.Name !== 'Workspace') throw new Error(workspace.Name);

		const part = Instance.new('Part');
		if (part.Parent !== null) throw new Error('a new instance is parented to nothing');

		part.Parent = workspace;
		if (part.Parent !== workspace) throw new Error('Parent did not round-trip');
		if (!part.IsDescendantOf(workspace)) throw new Error('not in the scene');
	)"));
}

TEST_CASE("parenting to an instance still nests", "[script]") {
	RegisterClasses();
	Store store("test.world");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// A part under a part is a real edge in the hierarchy — the tree is
	// organisational, so this decides nothing about drawing, but it is not a
	// courtesy either.
	REQUIRE(runtime->Run(R"(
		local model = Instance.new('Part')
		model.Name = 'Model'
		local child = Instance.new('Part')
		child.Parent = model
		assert(child.Parent ~= workspace, 'a child of a part is not a root')
		assert(child.Parent.Name == 'Model', child.Parent.Name)
	)"));
}

TEST_CASE("Name is a declared property in both languages", "[script]") {
	RegisterClasses();
	Store store("script_test");

	// It used to be special-cased in the Luau binding and absent from the
	// JavaScript one, which is the drift a declared property prevents.
	REQUIRE(MakeRuntime(store, Language::Luau)->Run(R"(
		local part = Instance.new('Part')
		part.Name = 'Orbiter'
		assert(part.Name == 'Orbiter', part.Name)
	)"));

	REQUIRE(MakeRuntime(store, Language::JavaScript)->Run(R"(
		const part = Instance.new('Part');
		part.Name = 'Orbiter';
		if (part.Name !== 'Orbiter') throw new Error(part.Name);
	)"));
}

TEST_CASE("Color3.fromRGB takes 0-255", "[script]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Color = Color3.fromRGB(255, 128, 0)
	)"));

	const Entity part = OnlyPart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Visual>(part)->Tint.R == Approx(1.0f).margin(0.01));
	CHECK(store.Get<Visual>(part)->Tint.B == Approx(0.0f).margin(0.01));
}

// --- the Universe's services ------------------------------------------------
//
// `game` is the universe and `workspace` is the world a script runs on. A
// script holds one `Store` and no binding hands it another, so the only way out
// of a world is a service that sits in the universe — which is rule 3 expressed
// as an API: nothing crossing a world boundary is a pointer.

TEST_CASE("MessagingService is reachable as a universe service", "[script]") {
	RegisterClasses();
	Store store("test.world");
	const auto runtime = MakeRuntime(store, Language::Luau);

	REQUIRE(runtime->Run(R"(
		local messaging = game:GetService('MessagingService')
		assert(messaging == MessagingService)
		assert(type(messaging.PublishAsync) == 'function')
		assert(type(messaging.SubscribeAsync) == 'function')
	)"));
}

TEST_CASE("a script has no route to another world", "[script]") {
	RegisterClasses();
	Store store("test.world");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// The invariant the whole arrangement rests on. `workspace` is *this*
	// world's, and there is no `game.Workspaces`, no `GetWorld`, no way to name
	// another one. If one ever appears, rule 3 is what it has to answer to.
	//
	// Asked of the store rather than of the name, because the name is
	// `Workspace` in every world now — it is the *instance* that is this
	// world's, and a name could no longer tell the two apart.
	REQUIRE(runtime->Run("assert(workspace:IsA('Workspace'))"));
	CHECK(engine::scene::WorkspaceOf(store) != engine::ecs::NULL_ENTITY);

	// Naming another world is an **error**, not a nil. That is stronger than
	// the check this used to make: a nil member is indistinguishable from a
	// member that exists and happens to be unset, and `game` refusing an
	// unknown name outright is also what Roblox does.
	CHECK_FALSE(runtime->Run("return game.GetWorld"));
	CHECK_FALSE(runtime->Run("return game.Workspaces"));
}

TEST_CASE("nil is spelled nil in both languages", "[script]") {
	RegisterClasses();
	Store store("test.world");

	// Luau has it as a keyword. JavaScript gets it as a global aliasing `null`,
	// because this is a Roblox-shaped API and a Roblox author writes
	// `part.Parent = nil`.
	// **`nil` detaches, and reads back as `nil`.** It used to read back as
	// `workspace`, because a null parent meant "a root of this world"; now it
	// means what it says, and the instance is in no scene until somebody says
	// otherwise.
	REQUIRE(MakeRuntime(store, Language::Luau)->Run(R"(
		local part = Instance.new('Part')
		part.Parent = workspace
		assert(part.Parent == workspace)

		part.Parent = nil
		assert(part.Parent == nil, 'nil detaches')
	)"));

	REQUIRE(MakeRuntime(store, Language::JavaScript)->Run(R"(
		if (nil !== null) throw new Error('nil is not null');
		const part = Instance.new('Part');
		part.Parent = workspace;
		part.Parent = nil;
		if (part.Parent !== null) throw new Error('nil detaches');
	)"));
}

TEST_CASE("a publish crosses from one world to another", "[script]") {
	RegisterClasses();

	// `Universe::Tick` dispatches its worlds through `Jobs`, so the pool has to
	// be up. Owned here rather than assumed: whether some earlier suite left
	// one running is not something this test gets to depend on.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(2);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	} pool;

	// Two worlds in one universe, which is the arrangement the whole design is
	// for. Neither script can see the other's store.
	engine::world::Universe universe;

	engine::world::WorldSettings sending;
	sending.Name = engine::core::Name("test.sender");
	engine::world::WorldSettings listening;
	listening.Name = engine::core::Name("test.listener");

	const engine::world::WorldId sender = universe.Create(sending);
	const engine::world::WorldId listener = universe.Create(listening);
	REQUIRE(sender.IsValid());
	REQUIRE(listener.IsValid());

	std::unique_ptr<engine::script::Runtime> speaker;
	std::unique_ptr<engine::script::Runtime> hearer;

	universe.Enter(listener, [&](Store &store) {
		hearer = MakeRuntime(store, Language::Luau);
		// The subscriber writes into the *world* rather than into a global.
		// Each chunk runs on its own sandboxed thread — one script cannot see
		// another's globals, which is deliberate — so the world is the only
		// place an assertion from C++ could read either way. It is also the
		// stronger claim: the message did not merely arrive, it changed
		// something.
		REQUIRE(hearer->Run(R"(
			MessagingService:SubscribeAsync('test.topic', function(message, topic)
				local marker = Instance.new('Part')
				marker.Name = message .. '/' .. topic
			end)
		)"));
	});

	// A subscription takes effect at the next barrier, so a message sent in the
	// same tick is not received — the honest answer, since the subscription did
	// not exist when it was sent.
	universe.Tick(1.0f / 60.0f);

	universe.Enter(sender, [&](Store &store) {
		speaker = MakeRuntime(store, Language::Luau);
		REQUIRE(speaker->Run("MessagingService:PublishAsync('test.topic', 'hello')"));
	});

	// The barrier that carries it across.
	universe.Tick(1.0f / 60.0f);

	bool received = false;
	universe.Enter(listener, [&](Store &store) {
		// Did the engine carry it? Checked apart from the binding so a failure
		// says which half is wrong.
		CHECK(engine::world::Postbox(store).Deliveries().size() == 1);

		hearer->Heartbeat(1.0f / 60.0f);

		store.Each<const engine::ecs::InstanceName>([&](Entity, const engine::ecs::InstanceName &name) {
			received = received || name.Value == engine::core::Name("hello/test.topic");
		});
	});

	// The message crossed a world boundary as bytes, which is the only way
	// anything crosses one.
	CHECK(received);

	// Torn down before the universe that owns the stores they point at.
	speaker.reset();
	hearer.reset();
}

TEST_CASE("TeleportService moves a player between two worlds", "[script]") {
	// **The bus operation that names a world, end to end.** `BusKind::Teleport`,
	// `Postbox::Teleport` and the router's delivery have all existed since v0.2
	// — and no script could reach any of it, and `PumpDeliveries` dropped every
	// arrival on the floor because it only ever looked for `Messaging`. So the
	// crossing worked and nothing could ask for one or notice one.
	//
	// Three claims, and each fails differently on its own:
	//
	//   * the player leaves the world they were in, or they are in two places;
	//   * the player arrives in the destination *with a body*, rebuilt from
	//     that world's own class table rather than carried across;
	//   * the data arrives with them, which is the only thing besides a name
	//     that crosses at all.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(2);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	} pool;

	engine::world::Universe universe;

	engine::world::WorldSettings lobbySettings;
	lobbySettings.Name = engine::core::Name("test.lobby");
	engine::world::WorldSettings arenaSettings;
	arenaSettings.Name = engine::core::Name("test.arena");

	const engine::world::WorldId lobby = universe.Create(lobbySettings);
	const engine::world::WorldId arena = universe.Create(arenaSettings);
	REQUIRE(lobby.IsValid());
	REQUIRE(arena.IsValid());

	std::unique_ptr<engine::script::Runtime> lobbyScripts;
	std::unique_ptr<engine::script::Runtime> arenaScripts;

	// Both worlds furnished, because a `Player` is a child of the `Players`
	// service — an unfurnished destination takes nobody, quietly, which is the
	// placeholder case and not this one.
	universe.Enter(lobby, [&](Store &store) {
		engine::scene::RegisterSceneClasses();
		engine::scene::InstallServices(store);
		lobbyScripts = MakeRuntime(store, Language::Luau);
	});
	universe.Enter(arena, [&](Store &store) {
		engine::scene::InstallServices(store);
		arenaScripts = MakeRuntime(store, Language::Luau);
	});

	Entity traveller;
	universe.Enter(lobby, [&](Store &store) {
		traveller = engine::scene::AddPlayer(store, "Wanderer");
		REQUIRE(traveller != engine::ecs::NULL_ENTITY);
		REQUIRE(engine::scene::LoadCharacter(store, traveller) != engine::ecs::NULL_ENTITY);

		REQUIRE(lobbyScripts->Run(R"(
			local player = game:GetService('Players'):FindFirstChild('Wanderer')
			TeleportService:Teleport('test.arena', player, { Score = 7 })
		)"));

		// **Gone from here on the call, not at the barrier.** The two worlds
		// cannot reach each other, so only this one can stop the player being
		// in both — and the router already holds a copy of the bytes.
		CHECK_FALSE(store.Alive(traveller));

		// **And their body with them, which is the half a screenshot notices.**
		// A `Player` is a row in a service and a character is a model in the
		// workspace; destroying the first and leaving the second is a body
		// standing in a world nobody is in, walking nowhere, for ever. It is
		// also what makes a round trip look like it duplicates people: one in
		// the world you left and one in the world you arrived in.
		CHECK(
			store.FindFirstChild(engine::scene::WorkspaceOf(store), "Wanderer") == engine::ecs::NULL_ENTITY
		);
	});

	// The barrier that carries it across.
	universe.Tick(1.0f / 60.0f);

	Entity arrived;
	universe.Enter(arena, [&](Store &store) {
		// Did the engine carry it? Checked apart from the binding, so a failure
		// says which half is wrong.
		REQUIRE(engine::world::Postbox(store).Deliveries().size() == 1);

		// **The arrival is admitted by the engine rather than by a script**, and
		// no longer by a *runtime* either — who is in a game is the host's
		// business, and a teleport that only worked in games whose author had
		// written a handler would be a feature with a footnote.
		//
		// **It used to be the Luau pump, and that was the footnote.** A
		// destination is chosen by a script in another world, so a world can be
		// somebody's destination without running a line of code: the studio's
		// unplayed second scene, a world whose scripts are JavaScript, a room
		// furnished by C++. Every one of them took the payload into its inbox
		// and left it there, and the traveller was destroyed in the world they
		// left and never built in the one they went to. `AdmitTeleports` is a
		// system on every world now, and this call is that system's body.
		CHECK(engine::script::AdmitTeleports(store) == 1);

		arrived = store.FindFirstChild(engine::scene::PlayersOf(store), "Wanderer");
		REQUIRE(arrived != engine::ecs::NULL_ENTITY);

		// **With a body, built here.** No entity crossed: this character is the
		// arena's own rows from the arena's own class table, which is what lets
		// two worlds disagree about what a `Player` is made of.
		const Entity character = engine::scene::CharacterOf(store, arrived);
		REQUIRE(character != engine::ecs::NULL_ENTITY);
		CHECK(store.Get<engine::scene::Character>(character) != nullptr);
	});

	// And the data came with them. Read through the same call a game would use,
	// which needs a `LocalPlayer` — the whole point of the name is that it is
	// nil on a server.
	universe.Enter(arena, [&](Store &store) {
		store.SetResource(engine::scene::LocalPlayer{arrived});
		REQUIRE(arenaScripts->Run(R"(
			local data = TeleportService:GetLocalPlayerTeleportData()
			local marker = Instance.new('Part')
			marker.Name = 'score:' .. tostring(data.Score)
		)"));

		bool found = false;
		store.Each<const engine::ecs::InstanceName>([&](Entity, const engine::ecs::InstanceName &name) {
			found = found || name.Value == engine::core::Name("score:7");
		});
		CHECK(found);
	});

	lobbyScripts.reset();
	arenaScripts.reset();
}

TEST_CASE("a world with no scripts still takes somebody in", "[script][teleport]") {
	// **The case that made an immersive cross-world portal delete you.** A
	// destination is chosen by a script in *another* world, so a world can be
	// somebody's destination without containing a line of code: the studio's
	// second scene, which is furnished but not being played; a world whose
	// scripts are JavaScript; a room built entirely by C++.
	//
	// Admitting an arrival used to happen inside the Luau runtime's own delivery
	// pump. So all three took the payload into their inbox and left it there —
	// and the source world had already destroyed the player, because only the
	// source world can. The traveller existed nowhere, the host that follows
	// them searched every world and found nobody, and the client was dropped as
	// lost. The portal drew the far room live the whole time, because a picture
	// is a draw list and needs no runtime.
	//
	// **Driven through the scheduler rather than by calling the function**, so
	// what is pinned is the wiring as well as the arithmetic:
	// `RegisterTeleportAdmission` is what every host installs and this is the
	// only test that proves it runs.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(2);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	} pool;

	engine::world::Universe universe;

	engine::world::WorldSettings fromSettings;
	fromSettings.Name = engine::core::Name("test.scripted");
	engine::world::WorldSettings toSettings;
	toSettings.Name = engine::core::Name("test.quiet");

	const engine::world::WorldId from = universe.Create(fromSettings);
	const engine::world::WorldId to = universe.Create(toSettings);
	REQUIRE(from.IsValid());
	REQUIRE(to.IsValid());

	std::unique_ptr<engine::script::Runtime> scripts;

	universe.Enter(from, [&](Store &store) {
		engine::scene::RegisterSceneClasses();
		engine::scene::InstallServices(store);
		scripts = MakeRuntime(store, Language::Luau);
	});

	// **The destination has services and a scheduler and no runtime at all.**
	// That is the whole point of the case: nothing here can run a line of Luau,
	// and somebody still has to be able to arrive.
	//
	// **Registered twice, deliberately.** `ecs::Scheduler` does not dedupe by
	// name, and a host that installs this once through a shared per-world call
	// and once beside its own systems is not hypothetical — the studio did
	// exactly that, and every arrival was admitted twice: two `Player` rows and
	// two characters per crossing, one adopted by the play link and one orphan
	// nobody drives, one more of them on every teleport. What stops it is that
	// the admitter *takes* what it admits out of the inbox, so the second copy
	// finds an empty list. This is the case that says so.
	universe.Enter(to, [&](Store &store, engine::ecs::Scheduler &systems) {
		engine::scene::InstallServices(store);
		engine::script::RegisterTeleportAdmission(systems);
		engine::script::RegisterTeleportAdmission(systems);
	});

	universe.Enter(from, [&](Store &store) {
		const Entity traveller = engine::scene::AddPlayer(store, "Wanderer");
		REQUIRE(traveller != engine::ecs::NULL_ENTITY);
		REQUIRE(engine::scene::LoadCharacter(store, traveller) != engine::ecs::NULL_ENTITY);

		REQUIRE(scripts->Run(R"(
			local player = game:GetService('Players'):FindFirstChild('Wanderer')
			TeleportService:Teleport('test.quiet', player, { Score = 7 })
		)"));

		CHECK_FALSE(store.Alive(traveller));
	});

	// One tick carries it across the barrier and the next runs the destination's
	// systems over what arrived. **Two, because the delivery lands in the inbox
	// at the barrier and `PreSimulation` has already run by then** — which is
	// the same one-tick gap `studio::PlayLink::Missing` exists to wait out.
	universe.Tick(1.0f / 60.0f);
	universe.Tick(1.0f / 60.0f);

	universe.Enter(to, [&](Store &store) {
		const Entity arrived = store.FindFirstChild(engine::scene::PlayersOf(store), "Wanderer");
		REQUIRE(arrived != engine::ecs::NULL_ENTITY);

		// With a body, built from this world's own class table — the same
		// guarantee a scripted destination gives, from a world that cannot run
		// a script to give it.
		CHECK(engine::scene::CharacterOf(store, arrived) != engine::ecs::NULL_ENTITY);

		// **Exactly one of them**, which is the half two registrations exist to
		// test. A second `Player` of the same name is one nothing claims and
		// nobody drives, and the character under it stands in the world for
		// ever.
		size_t people = 0;
		store.EachChild(engine::scene::PlayersOf(store), [&people](Entity) { people++; });
		CHECK(people == 1);

		size_t bodies = 0;
		store.EachChild(engine::scene::WorkspaceOf(store), [&](Entity child) {
			bodies += store.InstanceNameOf(child) == engine::core::Name("Wanderer") ? 1u : 0u;
		});
		CHECK(bodies == 1);
	});

	scripts.reset();
}
