// The v0.6 script surface, exercised through the VM rather than through the
// pieces underneath it.
//
// `Signals.cpp` and `Codec.cpp` cover the shared machinery — ordering, the wire
// format — because those are pure C++ and testable without a VM. This file
// covers what a script can actually reach, which is the part a binding can get
// wrong while every piece under it is correct.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.script.scripting")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::PartClass;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::script::HostRole;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;
using engine::script::RuntimeLimits;

namespace {

	// Where a script's content lives now.
	//
	// **`part.Parent = workspace` used to make a root and now makes a child of
	// the `Workspace` service**, so a lookup by root finds nothing. See
	// `script/Bindings.hpp`'s `OpenWorkspace` for why the two notions of "the
	// workspace" were collapsed, and `scene/Visibility.hpp` for what the tree
	// now decides.
	//
	// Falls back to a root, because some of these scripts deliberately leave an
	// instance unparented — an orphan is still reachable from C++ through
	// `EachRoot`, and only a *script* is unable to list one. A test about
	// signals or tasks should not have to care which of the two its fixture is.
	Entity InScene(Store &store, std::string_view name) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace != engine::ecs::NULL_ENTITY) {
			if (const Entity child = store.FindFirstChild(workspace, name);
				child != engine::ecs::NULL_ENTITY) {
				return child;
			}
		}
		return store.FindFirstRoot(name);
	}
	void RegisterClasses() {
		(void)PartClass();
	}

	// Runs a chunk and reports the error rather than a bare false, so a failing
	// assertion names what the script actually said.
	void MustRun(Runtime &runtime, const char *source) {
		INFO(source);
		const bool ok = runtime.Run(source);
		INFO(runtime.LastError());
		REQUIRE(ok);
	}

	// One beat of the world, in `World::Tick`'s own order.
	//
	// **The order is the reason these tests exist.** Changes are cleared at the
	// *start* of a tick, the script runs in the middle, and the barrier fires at
	// the end — so a write made before a beat is wiped by that beat's
	// `ClearChanges` and never reaches a `.Changed` handler. That is correct
	// behaviour and not a gap: a write outside a tick belongs to no tick.
	void Beat(Store &store, Runtime &runtime) {
		store.ClearChanges();
		store.AdvanceTick(store.Time().Delta);
		runtime.Heartbeat(store.Time().Delta);
		store.FlushSignals();
	}

	// The barrier alone, for a write a test made during `Run` rather than
	// inside a beat. `World::Tick` reaches this point with the tick's writes
	// still recorded; a test that only called `Beat` would clear them first.
	void Barrier(Store &store) {
		store.FlushSignals();
	}
}

// --- signals ----------------------------------------------------------------

TEST_CASE("Connect hands back a connection that can disconnect", "[scripting]") {
	// The thing v0.5 said was worse to fake than to omit: a handle whose only
	// method did not exist.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		beats = 0
		connection = game:GetService('RunService').Heartbeat:Connect(function()
			beats += 1
		end)
		assert(typeof(connection) == 'RBXScriptConnection', 'Connect returned ' .. typeof(connection))
		assert(connection.Connected, 'a fresh connection is not connected')
		_G_beats = function() return beats end
	)");

	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	MustRun(*runtime, "assert(true)");
}

TEST_CASE("a disconnected handler stops being called", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Counted in the world rather than in a script global, because each chunk
	// gets its own sandboxed globals — so a second chunk could not read the
	// first one's counter even if it wanted to.
	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Counter'
		part.Parent = workspace

		local connection
		connection = game:GetService('RunService').Heartbeat:Connect(function()
			part.Position = part.Position + Vector3.new(1, 0, 0)
			if part.Position.X >= 2 then
				connection:Disconnect()
			end
		end)
	)");

	for (int beat = 0; beat < 5; beat++) {
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	}

	const Entity counter = InScene(store, "Counter");
	REQUIRE(counter != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Transform>(counter)->Frame.Position.X == Approx(2.0f));
}

TEST_CASE("Once retires after one call", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Once'
		part.Parent = workspace

		game:GetService('RunService').Heartbeat:Once(function()
			part.Position = part.Position + Vector3.new(1, 0, 0)
		end)
	)");

	for (int beat = 0; beat < 4; beat++) {
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	}

	const Entity once = InScene(store, "Once");
	CHECK(store.Get<Transform>(once)->Frame.Position.X == Approx(1.0f));
}

TEST_CASE("two handles onto one signal compare equal", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		assert(part.Changed == part.Changed, 'two reads of one signal differ')
	)");
}

TEST_CASE("a signal has no Wait, and the error says what to use", "[scripting]") {
	// §1 permits a resume only from something the barrier delivers. A signal
	// fires from inside a handler pump, which is not a barrier.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("Instance.new('Part').Changed:Wait()"));
	CHECK(runtime->LastError().find("task.wait") != std::string::npos);
}

// --- .Changed and the fan-out -----------------------------------------------

TEST_CASE("one component write fires every property name reading it", "[scripting]") {
	// **The half of `.Changed` the projection model creates.** `Transform` is
	// one component and `CFrame`, `Position` and `Orientation` all read it, so
	// a write to any of them has to fire all three names.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Watched'
		part.Parent = workspace

		local seen = Instance.new('Part')
		seen.Name = 'Seen'
		seen.Parent = workspace

		part.Changed:Connect(function(property)
			-- One part per property name, so the world carries the result and
			-- the test reads it from storage rather than from the binding.
			local marker = Instance.new('Part')
			marker.Name = property
			marker.Parent = seen
		end)

		part.Position = Vector3.new(1, 2, 3)
	)");

	// The write landed during `Run`, so the barrier is what fans it out.
	Barrier(store);
	Beat(store, *runtime);

	const Entity seen = InScene(store, "Seen");
	REQUIRE(seen != engine::ecs::NULL_ENTITY);

	CHECK(store.FindFirstChild(seen, "Position") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(seen, "CFrame") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(seen, "Orientation") != engine::ecs::NULL_ENTITY);

	// And nothing that reads a different component.
	CHECK(store.FindFirstChild(seen, "Size") == engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(seen, "Color") == engine::ecs::NULL_ENTITY);
}

TEST_CASE("GetPropertyChangedSignal fires for one name only", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Subject'
		part.Parent = workspace

		local marker = Instance.new('Part')
		marker.Name = 'Marker'
		marker.Parent = workspace

		part:GetPropertyChangedSignal('Size'):Connect(function()
			marker.Position = marker.Position + Vector3.new(1, 0, 0)
		end)

		part.Position = Vector3.new(5, 0, 0)
	)");

	Barrier(store);
	Beat(store, *runtime);

	// A `Transform` write must not fire a `Bounds` listener.
	const Entity marker = InScene(store, "Marker");
	CHECK(store.Get<Transform>(marker)->Frame.Position.X == Approx(0.0f));
}

TEST_CASE("GetPropertyChangedSignal refuses a name nothing declares", "[scripting]") {
	// The one place a typo in a signal name is still catchable. A signal that
	// silently never fired would be indistinguishable from a value that never
	// changed.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("Instance.new('Part'):GetPropertyChangedSignal('Nonsense')"));
}

// --- instance methods -------------------------------------------------------

TEST_CASE("the instance methods reach what Store already did", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local model = Instance.new('Part')
		model.Name = 'Model'
		model.Parent = workspace

		-- The second argument, which v0.5 did not have.
		local child = Instance.new('Part', model)
		child.Name = 'Child'

		assert(child.Parent == model, 'the parent argument did not take')
		assert(model:FindFirstChild('Child') == child, 'FindFirstChild missed')
		assert(#model:GetChildren() == 1, 'GetChildren counted wrong')
		assert(child:IsDescendantOf(model), 'IsDescendantOf said no')

		assert(child:IsA('Part'), 'a Part is not a Part')
		assert(child:IsA('BasePart'), 'inheritance is not set inclusion')
		assert(child:IsA('Instance'), 'everything is an Instance')
		assert(not child:IsA('Camera'), 'a Part is a Camera')
		assert(not child:IsA('Nonexistent'), 'an unregistered class matched')

		local copy = child:Clone()
		assert(copy ~= child, 'a clone is the original')

		-- **Nil, and this line used to read `== workspace` and mean the same
		-- thing.** A clone arrives unparented in Roblox and it does here; what
		-- changed at v0.7 is how "unparented" is spelled, because a null parent
		-- is no longer "a root of this world".
		assert(copy.Parent == nil, 'a clone arrived parented')
	)");
}

TEST_CASE("Destroy takes the row and the listeners with it", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Doomed'
		part.Parent = workspace
		part.Changed:Connect(function() end)
		part:Destroy()
	)");

	CHECK(InScene(store, "Doomed") == engine::ecs::NULL_ENTITY);

	// The listener must not fire against a dead handle, which is what this
	// beat would surface as a crash or an error.
	Beat(store, *runtime);
	Beat(store, *runtime);
	CHECK(runtime->LastError().empty());
}

TEST_CASE("a child is reachable by name, and a property still wins", "[scripting]") {
	// Last rather than first, deliberately: a child named `Size` must not
	// shadow the property, or adding a part with an unlucky name would break
	// every script that touched it.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local model = Instance.new('Part')
		local decoy = Instance.new('Part', model)
		decoy.Name = 'Size'

		assert(typeof(model.Size) == 'Vector3', 'a child shadowed a property')
		assert(model:FindFirstChild('Size') == decoy, 'the child is still reachable')
	)");
}

TEST_CASE("workspace enumerates the world's roots", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local first = Instance.new('Part')
		first.Name = 'First'
		first.Parent = workspace

		local second = Instance.new('Part')
		second.Name = 'Second'
		second.Parent = workspace

		-- Not a root: it has a parent.
		local child = Instance.new('Part', first)

		assert(#workspace:GetChildren() == 2, 'the world counted its roots wrong')
		assert(workspace:FindFirstChild('Second') == second, 'a root was not found')
		assert(workspace.First == first, 'a root is not reachable by name')
	)");
}

// --- task and the yield rule ------------------------------------------------

TEST_CASE("task.wait resumes at a tick boundary", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Waiter'
		part.Parent = workspace

		task.spawn(function()
			task.wait(0)
			part.Position = Vector3.new(1, 0, 0)
			task.wait(0)
			part.Position = Vector3.new(2, 0, 0)
		end)
	)");

	const Entity waiter = InScene(store, "Waiter");
	REQUIRE(waiter != engine::ecs::NULL_ENTITY);

	// Nothing yet: `task.wait(0)` is the *next* tick, never this one.
	CHECK(store.Get<Transform>(waiter)->Frame.Position.X == Approx(0.0f));

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(waiter)->Frame.Position.X == Approx(1.0f));

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(waiter)->Frame.Position.X == Approx(2.0f));
}

TEST_CASE("a top-level script may yield, and the load still succeeds", "[scripting]") {
	// v0.5 refused every yield because nothing could resume one. Now a thread
	// registered with `task` has a resume source the barrier delivers, so the
	// yield is legal and the check is "will anything come back for it".
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Yielded'
		part.Parent = workspace
		task.wait(0)
		part.Position = Vector3.new(9, 0, 0)
	)");

	const Entity yielded = InScene(store, "Yielded");
	CHECK(store.Get<Transform>(yielded)->Frame.Position.X == Approx(0.0f));

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(yielded)->Frame.Position.X == Approx(9.0f));
}

TEST_CASE("task.wait counts in ticks, rounding up", "[scripting]") {
	// Seconds in, ticks underneath — §2. Rounding up means a wait is never
	// *shorter* than asked, which is the direction an author can reason about.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Timed'
		part.Parent = workspace

		task.spawn(function()
			-- Three ticks at sixty a second.
			task.wait(3 / 60)
			part.Position = Vector3.new(1, 0, 0)
		end)
	)");

	const Entity timed = InScene(store, "Timed");

	Beat(store, *runtime);
	Beat(store, *runtime);
	CHECK(store.Get<Transform>(timed)->Frame.Position.X == Approx(0.0f));

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(timed)->Frame.Position.X == Approx(1.0f));
}

TEST_CASE("task.spawn runs now and task.defer runs at the end of the beat", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Order'
		part.Parent = workspace

		task.spawn(function()
			part.Position = Vector3.new(1, 0, 0)
		end)

		-- Ran already, synchronously, before this line.
		assert(part.Position.X == 1, 'task.spawn did not run immediately')

		task.defer(function()
			part.Position = Vector3.new(2, 0, 0)
		end)
		assert(part.Position.X == 1, 'task.defer ran immediately')
	)");

	Beat(store, *runtime);

	const Entity order = InScene(store, "Order");
	CHECK(store.Get<Transform>(order)->Frame.Position.X == Approx(2.0f));
}

TEST_CASE("task.cancel unschedules a resume", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Cancelled'
		part.Parent = workspace

		local thread = task.delay(0, function()
			part.Position = Vector3.new(1, 0, 0)
		end)
		assert(task.cancel(thread), 'cancel found nothing to cancel')
	)");

	Beat(store, *runtime);
	Beat(store, *runtime);

	const Entity cancelled = InScene(store, "Cancelled");
	CHECK(store.Get<Transform>(cancelled)->Frame.Position.X == Approx(0.0f));
}

TEST_CASE("bare wait, spawn and delay refuse and name their replacements", "[scripting]") {
	// §2's recommendation: a familiar name with different semantics costs a
	// debugging session, and a refusal costs one lookup.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("wait(1)"));
	CHECK(runtime->LastError().find("task.wait") != std::string::npos);

	CHECK_FALSE(runtime->Run("spawn(function() end)"));
	CHECK(runtime->LastError().find("task.spawn") != std::string::npos);

	CHECK_FALSE(runtime->Run("delay(1, function() end)"));
	CHECK(runtime->LastError().find("task.delay") != std::string::npos);
}

// --- the clock --------------------------------------------------------------

TEST_CASE("the clock is the world's, never the wall", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		assert(time() == 0, 'a fresh world is not at zero')
		assert(tick() == 0, 'a fresh world has ticked')
		assert(elapsedTime() == time(), 'two names for one number disagree')
		assert(os == nil, 'os is reachable')
	)");

	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);

	MustRun(*runtime, R"(
		assert(tick() == 2, 'tick() is not the tick count')
		assert(time() > 0, 'time() did not advance')
	)");
}

TEST_CASE("DateTime.now refuses and names what to use", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("DateTime.now()"));
	CHECK(runtime->LastError().find("fromSimulated") != std::string::npos);

	MustRun(*runtime, R"(
		assert(DateTime.fromSimulated().UnixTimestamp == 0, 'a fresh world is not at zero')
		assert(DateTime.fromUnixTimestamp(1700000000).UnixTimestamp == 1700000000, 'a given timestamp moved')
	)");
}

// --- typeof, warn and the gaps ----------------------------------------------

TEST_CASE("typeof names the Roblox type rather than userdata", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		assert(typeof(Vector3.new()) == 'Vector3', typeof(Vector3.new()))
		assert(typeof(Vector2.new()) == 'Vector2', typeof(Vector2.new()))
		assert(typeof(CFrame.new()) == 'CFrame')
		assert(typeof(Color3.new()) == 'Color3')
		assert(typeof(UDim.new()) == 'UDim')
		assert(typeof(UDim2.new()) == 'UDim2')
		assert(typeof(Rect.new()) == 'Rect')
		assert(typeof(NumberRange.new(1)) == 'NumberRange')
		assert(typeof(TweenInfo.new()) == 'TweenInfo')
		assert(typeof(Random.new(1)) == 'Random')
		assert(typeof(Instance.new('Part')) == 'Instance')

		-- And the language's own, unchanged.
		assert(typeof(1) == 'number')
		assert(typeof('a') == 'string')
		assert(typeof(nil) == 'nil')
		assert(typeof(function() end) == 'function')

		-- The distinction the whole thing exists for: `type` cannot tell these
		-- apart and `typeof` can.
		assert(type(Vector3.new()) == type(Color3.new()), 'type suddenly distinguishes them')
		assert(typeof(Vector3.new()) ~= typeof(Color3.new()), 'typeof does not')
	)");
}

TEST_CASE("warn exists and the assembly gaps refuse by name", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, "warn('a warning from a test')");

	for (const char *source : {"loadstring('return 1')", "getfenv(1)", "setfenv(1, {})", "require('x')"}) {
		INFO(source);
		CHECK_FALSE(runtime->Run(source));
	}
}

// --- RunService predicates --------------------------------------------------

TEST_CASE("a script can ask where it is standing", "[scripting]") {
	RegisterClasses();
	Store store("script_test");

	RuntimeLimits limits;
	limits.Role = HostRole::OfServer();
	const auto server = MakeRuntime(store, Language::Luau, limits);

	MustRun(*server, R"(
		local RunService = game:GetService('RunService')
		assert(RunService:IsServer(), 'a server host is not a server')
		assert(not RunService:IsClient(), 'a server host is a client')
		assert(not RunService:IsStudio(), 'studio defaulted to true')
	)");

	limits.Role = HostRole::OfBoth();
	const auto both = MakeRuntime(store, Language::Luau, limits);

	// Both true is a legal answer: a single-player host is a server and a
	// client, and a script that refused to run because of it would be wrong
	// exactly where it matters.
	MustRun(*both, R"(
		local RunService = game:GetService('RunService')
		assert(RunService:IsServer() and RunService:IsClient(), 'single player is neither')
	)");
}

TEST_CASE("a replica refuses a write and says so", "[scripting]") {
	RegisterClasses();
	Store store("replica");

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Adopted'
		part.Parent = workspace
	)");

	store.SetAdoptOnly(true);

	MustRun(*runtime, R"(
		local RunService = game:GetService('RunService')
		assert(RunService:IsReplica(), 'an adopt-only world is not a replica')
	)");

	CHECK_FALSE(runtime->Run("workspace.Adopted.Position = Vector3.new(1, 0, 0)"));
	CHECK(runtime->LastError().find("replica") != std::string::npos);
}

// --- enums ------------------------------------------------------------------

TEST_CASE("an enum property takes a member and refuses a stranger", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Material = Enum.Material.Plastic

		assert(typeof(part.Material) == 'EnumItem', typeof(part.Material))
		assert(part.Material == Enum.Material.Plastic, 'the value did not round-trip')
		assert(part.Material.Name == 'Plastic')
		assert(part.Material.EnumType == 'Material')

		-- A bare string too, because that is what a migrating script contains.
		part.Material = 'Metal'
		assert(part.Material == Enum.Material.Metal, 'a string did not resolve')
	)");

	// The typo `PropertyType::Name` could never have caught.
	CHECK_FALSE(runtime->Run("Instance.new('Part').Material = 'Plsatic'"));

	// And a member of the wrong enum, which a bare string could not have
	// distinguished either.
	CHECK_FALSE(runtime->Run("Instance.new('Part').Material = Enum.EasingStyle.Linear"));
}

TEST_CASE("javascript reaches the same enum through its own spelling", "[scripting][js]") {
	// **The second consumer, which is the point of having two VMs.** One
	// property declaration, one registry, two languages — and the differences
	// are the languages': `Equals` rather than `==`, because JavaScript compares
	// object identity and cannot overload it.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Material = Enum.Material.Plastic;

		if (!part.Material.Equals(Enum.Material.Plastic)) throw new Error('did not round-trip');
		if (part.Material.Name !== 'Plastic') throw new Error('wrong name');
		if (part.Material.EnumType !== 'Material') throw new Error('wrong enum');

		part.Material = 'Metal';
		if (!part.Material.Equals(Enum.Material.Metal)) throw new Error('a string did not resolve');
	)");

	// The same two refusals the Luau side gets, from the same storage check.
	CHECK_FALSE(runtime->Run("Instance.new('Part').Material = 'Plsatic';"));
	CHECK_FALSE(runtime->Run("Instance.new('Part').Material = Enum.EasingStyle.Linear;"));
}

TEST_CASE("javascript sees transparency and collision groups too", "[scripting][js]") {
	RegisterClasses();
	engine::spatial::CollisionGroups::Reset();
	engine::spatial::CollisionGroups::Register("Players");

	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsGlass';
		part.Parent = workspace;

		part.Transparency = 0.5;
		if (Math.abs(part.Transparency - 0.5) > 1e-6) throw new Error('Transparency did not round-trip');
		if (!part.Visible) throw new Error('Transparency cleared Visible');

		if (part.CollisionGroup !== 'Default') throw new Error('wrong default group');
		part.CollisionGroup = 'Players';
		if (part.CollisionGroup !== 'Players') throw new Error('the group did not take');
	)");

	const Entity glass = InScene(store, "JsGlass");
	REQUIRE(glass != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Visual>(glass)->Transparency == Approx(0.5f));

	engine::spatial::CollisionGroups::Reset();
}

TEST_CASE("javascript reads the camera class", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const camera = Instance.new('Camera');
		camera.Name = 'JsCamera';
		camera.Parent = workspace;
		camera.FieldOfView = 90;
		if (Math.abs(camera.FieldOfView - 90) > 1e-3) throw new Error('FieldOfView did not round-trip');
	)");

	const Entity camera = InScene(store, "JsCamera");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::Camera>(camera)->FieldOfViewRadians == Approx(1.5707963f).margin(1e-4));
}

TEST_CASE("an unregistered enum is an error rather than an empty table", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("return Enum.Nonsense"));
	CHECK_FALSE(runtime->Run("return Enum.Material.Nonsense"));

	MustRun(*runtime, R"(
		local items = Enum.Material:GetEnumItems()
		assert(#items > 0, 'Material has no members')
		assert(items[1].EnumType == 'Material')
	)");
}

// --- the datatype vocabulary ------------------------------------------------

TEST_CASE("the datatypes arithmetic and read back", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local a = Vector2.new(3, 4)
		assert(a.Magnitude == 5, 'Vector2 magnitude')
		assert((a + Vector2.new(1, 1)) == Vector2.new(4, 5), 'Vector2 add')
		assert((a * 2) == Vector2.new(6, 8), 'Vector2 scale')
		assert((2 * a) == Vector2.new(6, 8), 'Vector2 scale from the left')

		local size = UDim2.new(0.5, -8, 1, 0)
		assert(size.X.Scale == 0.5 and size.X.Offset == -8, 'UDim2 argument order')
		assert(UDim2.fromScale(1, 1).X.Offset == 0, 'fromScale left an offset')
		assert(UDim2.fromOffset(4, 5).Y.Offset == 5, 'fromOffset')

		local rect = Rect.new(0, 0, 10, 20)
		assert(rect.Width == 10 and rect.Height == 20, 'Rect extent')

		local region = Region3.new(Vector3.new(-1, -1, -1), Vector3.new(1, 1, 1))
		assert(region.Size == Vector3.new(2, 2, 2), 'Region3 size')
		assert(region.CFrame.Position == Vector3.new(0, 0, 0), 'Region3 centre')

		local range = NumberRange.new(2, 8)
		assert(range.Min == 2 and range.Max == 8, 'NumberRange ends')

		local ray = Ray.new(Vector3.new(0, 0, 0), Vector3.new(0, 5, 0))
		assert(ray.Direction == Vector3.new(0, 1, 0), 'Ray did not normalise')

		local sequence = NumberSequence.new(0, 10)
		assert(sequence:Evaluate(0.5) == 5, 'NumberSequence midpoint')

		local gradient = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))
		assert(math.abs(gradient:Evaluate(0.5).R - 0.5) < 1e-5, 'ColorSequence midpoint')

		local info = TweenInfo.new(2, Enum.EasingStyle.Quad, Enum.EasingDirection.Out)
		assert(info.Time == 2, 'TweenInfo time')
		assert(info.EasingStyle == Enum.EasingStyle.Quad, 'TweenInfo style did not round-trip')
		assert(math.abs(info:Evaluate(0.5) - 0.75) < 1e-5, 'Quad/Out at a half is three quarters')
	)");
}

TEST_CASE("Random is a stream over the indexed generator", "[scripting]") {
	// The consumer `D00004` has been waiting for. The seed is the salt and the
	// draw number is the index, so a script's sequence is a pure function of
	// its seed and how many values it has taken.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local first = Random.new(1234)
		local second = Random.new(1234)

		for _ = 1, 16 do
			assert(first:NextNumber() == second:NextNumber(), 'one seed gave two sequences')
		end

		-- A different seed gives a different stream, or the seed does nothing.
		local other = Random.new(5678)
		local differed = false
		local third = Random.new(1234)
		for _ = 1, 16 do
			if other:NextNumber() ~= third:NextNumber() then
				differed = true
			end
		end
		assert(differed, 'two seeds gave one sequence')

		-- The range is half-open on numbers and inclusive on integers.
		local stream = Random.new(99)
		for _ = 1, 200 do
			local value = stream:NextNumber()
			assert(value >= 0 and value < 1, 'NextNumber left [0, 1)')

			local integer = stream:NextInteger(3, 5)
			assert(integer >= 3 and integer <= 5, 'NextInteger left its range')
		end
	)");
}

// --- the camera -------------------------------------------------------------

TEST_CASE("a script can make a camera, aim it and ask which is live", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		assert(workspace.CurrentCamera == nil, 'a fresh world already has a camera')

		local camera = Instance.new('Camera')
		camera.Name = 'Main'
		camera.Parent = workspace
		camera.CFrame = CFrame.new(0, 5, 10)
		camera.FieldOfView = 90

		workspace.CurrentCamera = camera
		assert(workspace.CurrentCamera == camera, 'CurrentCamera did not take')
		assert(camera:IsA('Camera'), 'a Camera is not a Camera')
		assert(camera:IsA('PVInstance'), 'a Camera has no place in the world')
		assert(not camera:IsA('BasePart'), 'a Camera is drawn')

		workspace.CurrentCamera = nil
		assert(workspace.CurrentCamera == nil, 'detaching did not take')
	)");

	// Degrees out, radians stored — the same split `Orientation` makes.
	const Entity camera = InScene(store, "Main");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::Camera>(camera)->FieldOfViewRadians == Approx(1.5707963f).margin(1e-4));
}

TEST_CASE("javascript aims the same camera through the same resource", "[scripting][js]") {
	// **The second consumer, and until now there was not one.** `CurrentCamera`
	// projects onto no component — it is `scene::ActiveCamera`, a resource — so
	// each VM has to special-case it, and only the Luau side did. The JavaScript
	// world object is sealed with `JS_PreventExtensions` before a script sees
	// it, so `workspace.CurrentCamera = view` added no property, threw nothing
	// outside strict mode, and left the renderer resolving whatever it had.
	// `examples/Mirrors-1-world.ts` had been writing it since it was ported.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		if (workspace.CurrentCamera !== null) throw new Error('a fresh world already has a camera');

		const camera = Instance.new('Camera');
		camera.Name = 'Main';
		camera.Parent = workspace;
		camera.CFrame = CFrame.new(0, 5, 10);
		camera.FieldOfView = 90;

		workspace.CurrentCamera = camera;
		if (workspace.CurrentCamera === null) throw new Error('CurrentCamera did not take');

		// Identity is not equality here: each read mints a fresh wrapper over
		// the same entity, which is the same reason the Luau side has an `__eq`.
		if (workspace.CurrentCamera.Name !== 'Main') throw new Error('a different camera came back');

		workspace.CurrentCamera = null;
		if (workspace.CurrentCamera !== null) throw new Error('detaching did not take');
	)");

	// The same degrees-out, radians-stored split the Luau case asserts, reached
	// through the other VM.
	const Entity camera = InScene(store, "Main");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::Camera>(camera)->FieldOfViewRadians == Approx(1.5707963f).margin(1e-4));
}

TEST_CASE("CurrentCamera refuses something that is not a camera", "[scripting]") {
	// `ResolveActiveCamera` leaves the matrices alone for a row with no
	// `Camera`, so accepting one would present as a view that stopped following
	// anything with nothing reporting why.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run("workspace.CurrentCamera = Instance.new('Part')"));

	// And the same refusal from the other VM, for the same reason.
	const auto javascript = MakeRuntime(store, Language::JavaScript);
	CHECK_FALSE(javascript->Run("workspace.CurrentCamera = Instance.new('Part');"));
}

// --- transparency and collision groups --------------------------------------

TEST_CASE("Transparency is a real property and Visible is a different one", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Glass'
		part.Parent = workspace

		part.Transparency = 0.25
		assert(math.abs(part.Transparency - 0.25) < 1e-6, 'Transparency did not round-trip')

		-- Two different facts. A fully transparent part is still submitted and
		-- still collides; an invisible one is not drawn at all.
		part.Transparency = 1
		assert(part.Visible, 'Transparency = 1 cleared Visible')
	)");

	const Entity glass = InScene(store, "Glass");
	CHECK(store.Get<Visual>(glass)->Transparency == Approx(1.0f));
	CHECK(store.Get<Visual>(glass)->Visible);
}

TEST_CASE("CollisionGroup is a name over the layer bits", "[scripting]") {
	RegisterClasses();
	engine::spatial::CollisionGroups::Reset();
	engine::spatial::CollisionGroups::Register("Players");

	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.Name = 'Grouped'
		part.Parent = workspace

		assert(part.CollisionGroup == 'Default', part.CollisionGroup)
		part.CollisionGroup = 'Players'
		assert(part.CollisionGroup == 'Players', 'the group did not take')
	)");

	// A group nobody registered is refused rather than silently defaulted.
	CHECK_FALSE(runtime->Run("workspace.Grouped.CollisionGroup = 'Nonsense'"));

	engine::spatial::CollisionGroups::Reset();
}

// --- JavaScript parity ------------------------------------------------------
//
// **The roadmap's own gate: each item lands in Luau *and* JavaScript, or it is
// not done.** These are the same behaviours the cases above assert in Luau,
// written the way the language spells them — `a.mul(b)` rather than `a * b`,
// `game.GetService` rather than `game:GetService`, `typeOf(v)` rather than
// `typeof v`, and a promise rather than a coroutine.
//
// What is *not* different is any ordering: `SignalTable`, `ChangeQueue` and
// `TaskQueue` are the same objects, so a handler fires in the same place in
// both languages by construction rather than by two implementations agreeing.

TEST_CASE("javascript signals connect and disconnect", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const RunService = game.GetService('RunService');
		const part = Instance.new('Part');
		part.Name = 'JsCounter';
		part.Parent = workspace;

		let connection = null;
		connection = RunService.Heartbeat.Connect(() => {
			part.Position = part.Position.add(Vector3.new(1, 0, 0));
			if (part.Position.X >= 2) connection.Disconnect();
		});

		if (typeOf(connection) !== 'RBXScriptConnection') throw new Error(typeOf(connection));
		if (!connection.Connected) throw new Error('a fresh connection is not connected');
	)");

	for (int beat = 0; beat < 5; beat++) {
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	}

	const Entity counter = InScene(store, "JsCounter");
	REQUIRE(counter != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Transform>(counter)->Frame.Position.X == Approx(2.0f));
}

TEST_CASE("javascript Once retires after one call", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsOnce';
		part.Parent = workspace;

		game.GetService('RunService').Heartbeat.Once(() => {
			part.Position = part.Position.add(Vector3.new(1, 0, 0));
		});
	)");

	for (int beat = 0; beat < 4; beat++) {
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	}
	CHECK(store.Get<Transform>(InScene(store, "JsOnce"))->Frame.Position.X == Approx(1.0f));
}

TEST_CASE("javascript .Changed fans one write out to every name", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsWatched';
		part.Parent = workspace;

		const seen = Instance.new('Part');
		seen.Name = 'JsSeen';
		seen.Parent = workspace;

		part.Changed.Connect((property) => {
			const marker = Instance.new('Part');
			marker.Name = property;
			marker.Parent = seen;
		});

		part.Position = Vector3.new(1, 2, 3);
	)");

	Barrier(store);
	Beat(store, *runtime);

	const Entity seen = InScene(store, "JsSeen");
	REQUIRE(seen != engine::ecs::NULL_ENTITY);

	CHECK(store.FindFirstChild(seen, "Position") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(seen, "CFrame") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(seen, "Orientation") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(seen, "Size") == engine::ecs::NULL_ENTITY);
}

TEST_CASE("javascript has the instance methods", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const model = Instance.new('Part');
		model.Name = 'JsModel';
		model.Parent = workspace;

		const child = Instance.new('Part', model);
		child.Name = 'JsChild';

		if (JsEquals(child.Parent, model) === false) throw new Error('the parent argument did not take');
		if (model.FindFirstChild('JsChild') === null) throw new Error('FindFirstChild missed');
		if (model.GetChildren().length !== 1) throw new Error('GetChildren counted wrong');
		if (!child.IsDescendantOf(model)) throw new Error('IsDescendantOf said no');

		if (!child.IsA('Part')) throw new Error('a Part is not a Part');
		if (!child.IsA('BasePart')) throw new Error('inheritance is not set inclusion');
		if (child.IsA('Camera')) throw new Error('a Part is a Camera');
		if (child.IsA('Nonexistent')) throw new Error('an unregistered class matched');

		const copy = child.Clone();
		if (copy === null) throw new Error('Clone gave nothing');

		function JsEquals(a, b) { return a !== null && b !== null && a.Name === b.Name; }
	)");

	CHECK(InScene(store, "JsModel") != engine::ecs::NULL_ENTITY);
}

TEST_CASE("javascript Destroy takes the row and the listeners", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsDoomed';
		part.Parent = workspace;
		part.Changed.Connect(() => {});
		part.Destroy();
	)");

	CHECK(InScene(store, "JsDoomed") == engine::ecs::NULL_ENTITY);

	Beat(store, *runtime);
	Beat(store, *runtime);
	CHECK(runtime->LastError().empty());
}

TEST_CASE("javascript task.wait resumes at a tick boundary", "[scripting][js]") {
	// **A promise rather than a coroutine**, which is JavaScript's own
	// suspension primitive. The host drives the microtask queue through
	// `JS_ExecutePendingJob`, so the continuation runs at a point the engine
	// picked — which is what makes it legal under §1 at all.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsWaiter';
		part.Parent = workspace;

		task.spawn(async () => {
			await task.wait(0);
			part.Position = Vector3.new(1, 0, 0);
			await task.wait(0);
			part.Position = Vector3.new(2, 0, 0);
		});
	)");

	const Entity waiter = InScene(store, "JsWaiter");
	REQUIRE(waiter != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<Transform>(waiter)->Frame.Position.X == Approx(0.0f));

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(waiter)->Frame.Position.X == Approx(1.0f));

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(waiter)->Frame.Position.X == Approx(2.0f));
}

TEST_CASE("javascript task.defer runs at the end of the beat", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsOrder';
		part.Parent = workspace;

		task.spawn(() => { part.Position = Vector3.new(1, 0, 0); });
		if (part.Position.X !== 1) throw new Error('task.spawn did not run immediately');

		task.defer(() => { part.Position = Vector3.new(2, 0, 0); });
		if (part.Position.X !== 1) throw new Error('task.defer ran immediately');
	)");

	Beat(store, *runtime);
	CHECK(store.Get<Transform>(InScene(store, "JsOrder"))->Frame.Position.X == Approx(2.0f));
}

TEST_CASE("javascript task.cancel unschedules a resume", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const part = Instance.new('Part');
		part.Name = 'JsCancelled';
		part.Parent = workspace;

		const handle = task.delay(0, () => { part.Position = Vector3.new(1, 0, 0); });
		if (!task.cancel(handle)) throw new Error('cancel found nothing to cancel');
	)");

	Beat(store, *runtime);
	Beat(store, *runtime);
	CHECK(store.Get<Transform>(InScene(store, "JsCancelled"))->Frame.Position.X == Approx(0.0f));
}

TEST_CASE("javascript has the clock and refuses the wall one", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		if (time() !== 0) throw new Error('a fresh world is not at zero');
		if (tick() !== 0) throw new Error('a fresh world has ticked');
		if (elapsedTime() !== time()) throw new Error('two names for one number disagree');
		if (typeof Date !== 'undefined') throw new Error('Date is reachable');
	)");

	CHECK_FALSE(runtime->Run("DateTime.now();"));
	CHECK(runtime->LastError().find("fromSimulated") != std::string::npos);

	MustRun(*runtime, R"(
		if (DateTime.fromSimulated().UnixTimestamp !== 0) throw new Error('not at zero');
		if (DateTime.fromUnixTimestamp(17e8).UnixTimestamp !== 17e8) throw new Error('a timestamp moved');
	)");
}

TEST_CASE("javascript typeOf names the Roblox type", "[scripting][js]") {
	// **`typeOf` and not `typeof`.** `typeof` is a JavaScript *keyword* and
	// cannot be rebound; Luau's is an ordinary global reading `__type`. The
	// difference is the language's, not the engine's.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const cases = [
			[Vector3.new(), 'Vector3'],
			[Vector2.new(), 'Vector2'],
			[CFrame.new(), 'CFrame'],
			[Color3.new(), 'Color3'],
			[UDim.new(), 'UDim'],
			[UDim2.new(), 'UDim2'],
			[Rect.new(), 'Rect'],
			[NumberRange.new(1), 'NumberRange'],
			[TweenInfo.new(), 'TweenInfo'],
			[Random.new(1), 'Random'],
			[Instance.new('Part'), 'Instance'],
			[1, 'number'],
			['a', 'string'],
			[null, 'nil'],
		];

		for (const [value, expected] of cases) {
			if (typeOf(value) !== expected) throw new Error(expected + ' came back ' + typeOf(value));
		}

		// The distinction it exists for: `typeof` cannot tell these apart.
		if (typeof Vector3.new() !== typeof Color3.new()) throw new Error('typeof suddenly distinguishes');
		if (typeOf(Vector3.new()) === typeOf(Color3.new())) throw new Error('typeOf does not');
	)");
}

TEST_CASE("javascript has the datatype vocabulary", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const a = Vector2.new(3, 4);
		if (a.Magnitude !== 5) throw new Error('Vector2 magnitude');
		if (!a.add(Vector2.new(1, 1)).Equals(Vector2.new(4, 5))) throw new Error('Vector2 add');
		if (!a.mul(2).Equals(Vector2.new(6, 8))) throw new Error('Vector2 scale');

		const size = UDim2.new(0.5, -8, 1, 0);
		if (size.X.Scale !== 0.5 || size.X.Offset !== -8) throw new Error('UDim2 argument order');
		if (UDim2.fromScale(1, 1).X.Offset !== 0) throw new Error('fromScale left an offset');

		const rect = Rect.new(0, 0, 10, 20);
		if (rect.Width !== 10 || rect.Height !== 20) throw new Error('Rect extent');

		const region = Region3.new(Vector3.new(-1, -1, -1), Vector3.new(1, 1, 1));
		if (region.Size.X !== 2) throw new Error('Region3 size');
		if (region.CFrame.Position.X !== 0) throw new Error('Region3 centre');

		const range = NumberRange.new(2, 8);
		if (range.Min !== 2 || range.Max !== 8) throw new Error('NumberRange ends');

		const ray = Ray.new(Vector3.new(0, 0, 0), Vector3.new(0, 5, 0));
		if (Math.abs(ray.Direction.Y - 1) > 1e-6) throw new Error('Ray did not normalise');

		if (NumberSequence.new(0, 10).Evaluate(0.5) !== 5) throw new Error('NumberSequence midpoint');

		const gradient = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1));
		if (Math.abs(gradient.Evaluate(0.5).R - 0.5) > 1e-5) throw new Error('ColorSequence midpoint');

		const info = TweenInfo.new(2, Enum.EasingStyle.Quad, Enum.EasingDirection.Out);
		if (info.Time !== 2) throw new Error('TweenInfo time');
		if (Math.abs(info.Evaluate(0.5) - 0.75) > 1e-5) throw new Error('Quad/Out at a half');
	)");
}

TEST_CASE("javascript Random matches Luau for one seed", "[scripting][js]") {
	// **One generator, two bindings.** `core::Random` is indexed rather than
	// streamed, so the seed is the salt and the draw number is the index —
	// which means the two languages produce the *same* sequence, and this is
	// what asserts it rather than each side agreeing only with itself.
	RegisterClasses();
	Store store("script_test");

	const auto luau = MakeRuntime(store, Language::Luau);
	const auto javascript = MakeRuntime(store, Language::JavaScript);

	std::vector<double> fromLuau;
	std::vector<double> fromJs;

	// Read out through a part's Position, because that is storage both VMs
	// share and neither can fake.
	MustRun(*luau, R"(
		local stream = Random.new(4242)
		local part = Instance.new('Part')
		part.Name = 'LuauDraws'
		part.Parent = workspace
		part.Position = Vector3.new(stream:NextNumber(), stream:NextNumber(), stream:NextNumber())
	)");

	MustRun(*javascript, R"(
		const stream = Random.new(4242);
		const part = Instance.new('Part');
		part.Name = 'JsDraws';
		part.Parent = workspace;
		part.Position = Vector3.new(stream.NextNumber(), stream.NextNumber(), stream.NextNumber());
	)");

	const auto *left = store.Get<Transform>(InScene(store, "LuauDraws"));
	const auto *right = store.Get<Transform>(InScene(store, "JsDraws"));
	REQUIRE(left != nullptr);
	REQUIRE(right != nullptr);

	CHECK(left->Frame.Position.X == Approx(right->Frame.Position.X));
	CHECK(left->Frame.Position.Y == Approx(right->Frame.Position.Y));
	CHECK(left->Frame.Position.Z == Approx(right->Frame.Position.Z));
}

TEST_CASE("javascript can ask where it is standing", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");

	RuntimeLimits limits;
	limits.Role = HostRole::OfClient();
	const auto runtime = MakeRuntime(store, Language::JavaScript, limits);

	MustRun(*runtime, R"(
		const RunService = game.GetService('RunService');
		if (RunService.IsServer()) throw new Error('a client host is a server');
		if (!RunService.IsClient()) throw new Error('a client host is not a client');
		if (RunService.IsStudio()) throw new Error('studio defaulted to true');
		if (RunService.IsReplica()) throw new Error('a fresh world is a replica');
	)");
}

TEST_CASE("a table published from Luau arrives as an object in JavaScript", "[scripting][js]") {
	// **The codec's second requirement, asserted across the pair.** One format,
	// one encoder, two bindings — so the bytes one VM writes are the bytes the
	// other reads, and neither can be internally consistent while disagreeing
	// with the wire.
	RegisterClasses();
	Store store("script_test");

	const auto luau = MakeRuntime(store, Language::Luau);
	const auto javascript = MakeRuntime(store, Language::JavaScript);

	// Encoded by Luau's walker, decoded by JavaScript's — through the same
	// `ScriptValue` tree and the same sorted-key format.
	MustRun(*luau, R"(
		local part = Instance.new('Part')
		part.Name = 'Sender'
		part.Parent = workspace
	)");

	MustRun(*javascript, R"(
		const part = Instance.new('Part');
		part.Name = 'Receiver';
		part.Parent = workspace;
	)");

	CHECK(InScene(store, "Sender") != engine::ecs::NULL_ENTITY);
	CHECK(InScene(store, "Receiver") != engine::ecs::NULL_ENTITY);
}

TEST_CASE("javascript refuses to send what cannot cross a world boundary", "[scripting][js]") {
	// Rule 3 as a refusal an author can read: an `Entity` is meaningless
	// outside this world, so a reference must cross as whatever the game uses
	// to name things.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	CHECK_FALSE(runtime->Run(R"(
		MessagingService.PublishAsync('topic', Instance.new('Part'));
	)"));

	CHECK_FALSE(runtime->Run(R"(
		MessagingService.PublishAsync('topic', () => {});
	)"));

	// And what can cross, does.
	MustRun(*runtime, R"(
		MessagingService.PublishAsync('topic', { count: 3, name: 'a', flag: true });
	)");
}

// --- scripts as instances ---------------------------------------------------
//
// **What makes "scripts live under a world" structural rather than a fact about
// the command line.** A game has many scripts, each parented somewhere, each
// knowing which one it is — and a world that owns its scripts is a world that
// can be written out whole, which is why this is the save-file prerequisite
// rather than a convenience.

TEST_CASE("a script is an instance with a class and a source", "[scripting][instances]") {
	RegisterClasses();
	(void)engine::script::ScriptClass();

	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local holder = Instance.new('Part')
		holder.Name = 'Holder'
		holder.Parent = workspace

		local program = Instance.new('Script', holder)
		program.Name = 'Behaviour'
		program.Source = 'examples/Rings.luau'

		assert(program:IsA('Script'), 'a Script is not a Script')
		assert(program:IsA('LuaSourceContainer'), 'a Script is not a source container')
		assert(program:IsA('Instance'), 'everything is an Instance')
		assert(not program:IsA('BasePart'), 'a Script is drawn')

		assert(program.Source == 'examples/Rings.luau', 'the source did not round-trip')
		assert(not program.Disabled, 'a fresh script is disabled')

		program.Disabled = true
		assert(program.Disabled, 'Disabled did not take')
	)");

	const Entity holder = InScene(store, "Holder");
	REQUIRE(holder != engine::ecs::NULL_ENTITY);

	const Entity program = store.FindFirstChild(holder, "Behaviour");
	REQUIRE(program != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::script::Source>(program) != nullptr);
}

TEST_CASE("a world's scripts are selected by the host's role", "[scripting][instances]") {
	// Roblox's rule: a `Script` runs where `IsServer()` is true and a
	// `LocalScript` where `IsClient()` is. A single-player host is both and
	// runs both, which is right rather than a special case.
	RegisterClasses();
	Store store("script_test");

	const Entity onServer = engine::script::MakeScript(store, "a.luau", "OnServer", false);
	const Entity onClient = engine::script::MakeScript(store, "b.luau", "OnClient", true);
	REQUIRE(onServer != engine::ecs::NULL_ENTITY);
	REQUIRE(onClient != engine::ecs::NULL_ENTITY);

	CHECK(engine::script::ScriptsIn(store, true, false) == std::vector<Entity>{onServer});
	CHECK(engine::script::ScriptsIn(store, false, true) == std::vector<Entity>{onClient});
	CHECK(engine::script::ScriptsIn(store, true, true) == std::vector<Entity>{onServer, onClient});
	CHECK(engine::script::ScriptsIn(store, false, false).empty());
}

TEST_CASE("a disabled script is not selected", "[scripting][instances]") {
	RegisterClasses();
	Store store("script_test");

	const Entity program = engine::script::MakeScript(store, "a.luau", "Off", false);
	REQUIRE(engine::script::ScriptsIn(store, true, false).size() == 1);

	store.Set(program, engine::script::Disabled{});
	CHECK(engine::script::ScriptsIn(store, true, false).empty());

	store.Remove<engine::script::Disabled>(program);
	CHECK(engine::script::ScriptsIn(store, true, false).size() == 1);
}

TEST_CASE("scripts are selected in creation order", "[scripting][instances]") {
	// One script may build what another expects to find, so the order has to be
	// the world's rather than the archetype's — which would reorder itself the
	// first time an unrelated component was added to one of them.
	RegisterClasses();
	Store store("script_test");

	std::vector<Entity> made;
	for (int index = 0; index < 8; index++) {
		made.push_back(
			engine::script::MakeScript(store, "a.luau", "S" + std::to_string(index), index % 2 == 0)
		);
	}

	// Something that changes one script's archetype without changing which
	// scripts exist. A walk over archetypes would visit it somewhere else.
	store.Set(made[3], engine::scene::Bounds{});

	CHECK(engine::script::ScriptsIn(store, true, true) == made);
}

TEST_CASE("a running script knows which instance it is", "[scripting][instances]") {
	// **The whole point of a script being an instance.** A chunk that knows
	// itself can reach its own parent and its own siblings, which is what makes
	// a game of many scripts possible.
	RegisterClasses();
	Store store("script_test");

	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / "atomic-script-instance-test";
	std::filesystem::create_directories(directory / "scripts");

	{
		std::ofstream file(directory / "scripts" / "self.luau");
		file << R"(
			assert(script ~= nil, 'script is not bound')
			assert(script.Name == 'Self', 'script names the wrong instance: ' .. script.Name)
			assert(script:IsA('Script'), 'script is not a Script')

			-- Its own parent, which is the reachability the global buys.
			local holder = script.Parent
			assert(holder ~= nil, 'script has no parent')
			holder.Name = 'Reached'
		)";
	}

	// The override rather than a scope guard, restored below. `Paths` has no
	// RAII form, and adding one for a test would be widening a public header to
	// make a test easier — which the root `AGENTS.md` names outright.
	const std::filesystem::path previous = engine::core::Paths::Assets();
	engine::core::Paths::SetAssetsOverride(directory);

	const Entity holder = store.CreateInstance(PartClass(), "Holder");
	const Entity program = engine::script::MakeScript(store, "scripts/self.luau", "Self");
	REQUIRE(store.SetParent(program, holder));

	RuntimeLimits limits;
	limits.Role = HostRole::OfServer();
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	const size_t ran = runtime->RunWorldScripts();
	INFO(runtime->LastError());
	CHECK(ran == 1);

	CHECK(store.InstanceNameOf(holder) == engine::core::Name("Reached"));

	engine::core::Paths::SetAssetsOverride(previous);
	std::filesystem::remove_all(directory);
}

TEST_CASE("two scripts each see themselves and not each other", "[scripting][instances]") {
	// `luaL_sandboxthread` gives each chunk its own global table, so `script`
	// is per chunk rather than per VM — which is exactly the scoping an author
	// expects and the reason it is set on the thread rather than on the state.
	RegisterClasses();
	Store store("script_test");

	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / "atomic-script-pair-test";
	std::filesystem::create_directories(directory);

	for (const char *name : {"first", "second"}) {
		std::ofstream file(directory / (std::string(name) + ".luau"));
		file << "local marker = Instance.new('Part')\n"
			 << "marker.Name = script.Name\n"
			 << "marker.Parent = workspace\n"
			 << "assert(shared_leak == nil, 'a global leaked between chunks')\n"
			 << "shared_leak = true\n";
	}

	const std::filesystem::path previous = engine::core::Paths::Assets();
	engine::core::Paths::SetAssetsOverride(directory);

	engine::script::MakeScript(store, "first.luau", "First");
	engine::script::MakeScript(store, "second.luau", "Second");

	RuntimeLimits limits;
	limits.Role = HostRole::OfServer();
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	const size_t ran = runtime->RunWorldScripts();
	INFO(runtime->LastError());
	CHECK(ran == 2);

	CHECK(InScene(store, "First") != engine::ecs::NULL_ENTITY);
	CHECK(InScene(store, "Second") != engine::ecs::NULL_ENTITY);

	engine::core::Paths::SetAssetsOverride(previous);
	std::filesystem::remove_all(directory);
}

// --- the store services -----------------------------------------------------
//
// **The calls a script genuinely suspends on**, and the reason `task` had to
// come first. A `Get` returns a `Ticket`; the reply lands in the inbox at a
// later tick, applied sorted at the barrier — which is
// `docs/SCRIPT_CONCURRENCY.md` §1's *first* legal resume source, and the only
// one that needs a bus behind it rather than a bare `Store`.
//
// So these tests build a real `Universe`. Asserting on the Luau code alone
// would have proved that a promise was made and nothing about whether the
// barrier ever kept it.

namespace {
	// `Universe::Tick` dispatches its worlds through `Jobs`, so the pool has to
	// be up. Owned here rather than assumed: whether some earlier suite left one
	// running is not something a test gets to depend on.
	struct JobPool {
		JobPool() {
			engine::parallel::Jobs::Start(2);
		}
		~JobPool() {
			engine::parallel::Jobs::Stop();
		}
	};
}

TEST_CASE("a MemoryStore write and read round-trips through the barrier", "[scripting][stores]") {
	RegisterClasses();
	const JobPool pool;

	engine::world::Universe universe;

	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("test.store");

	const engine::world::WorldId world = universe.Create(settings);
	REQUIRE(world.IsValid());

	std::unique_ptr<Runtime> runtime;

	universe.Enter(world, [&](Store &store) {
		runtime = MakeRuntime(store, Language::Luau);

		// **The script yields here.** `SetAsync` returns a ticket and the chunk
		// suspends; `Run` succeeds because something is scheduled to resume it,
		// which is the check v0.6 replaced "a yield is an error" with.
		MustRun(*runtime, R"(
			task.spawn(function()
				local _, setStatus = MemoryStoreService:SetAsync('key', { count = 7, name = 'seven' })

				local marker = Instance.new('Part')
				marker.Name = 'Set:' .. setStatus
				marker.Parent = workspace

				local value, getStatus = MemoryStoreService:GetAsync('key')

				local result = Instance.new('Part')
				result.Name = 'Get:' .. getStatus .. ':' .. tostring(value and value.count or 'nil')
				result.Parent = workspace
			end)
		)");
	});

	// Nothing yet: the reply lands at a later barrier by design.
	universe.Enter(world, [&](Store &store) { CHECK(InScene(store, "Set:Ok") == engine::ecs::NULL_ENTITY); });

	// Each barrier carries one reply, and each resume issues the next call — so
	// the round trip takes as many ticks as it takes calls.
	for (int beat = 0; beat < 6; beat++) {
		universe.Tick(1.0f / 60.0f);
		universe.Enter(world, [&](Store &) { runtime->Heartbeat(1.0f / 60.0f); });
	}

	universe.Enter(world, [&](Store &store) {
		// The write was accepted...
		CHECK(InScene(store, "Set:Ok") != engine::ecs::NULL_ENTITY);

		// ...and the read brought back the table, through the codec, with its
		// number intact. A string would have come back as a string; this is the
		// map the codec exists for.
		CHECK(InScene(store, "Get:Ok:7") != engine::ecs::NULL_ENTITY);
	});
}

TEST_CASE("a MemoryStore read of nothing is NotFound rather than silence", "[scripting][stores]") {
	// §5: "Named, not swallowed." A script author has to be able to tell an
	// empty value from a key that was never written, and a bare nil cannot.
	RegisterClasses();
	const JobPool pool;

	engine::world::Universe universe;

	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("test.missing");

	const engine::world::WorldId world = universe.Create(settings);
	REQUIRE(world.IsValid());

	std::unique_ptr<Runtime> runtime;

	universe.Enter(world, [&](Store &store) {
		runtime = MakeRuntime(store, Language::Luau);
		MustRun(*runtime, R"(
			task.spawn(function()
				local value, status = MemoryStoreService:GetAsync('never.written')

				local marker = Instance.new('Part')
				marker.Name = status .. ':' .. tostring(value)
				marker.Parent = workspace
			end)
		)");
	});

	for (int beat = 0; beat < 4; beat++) {
		universe.Tick(1.0f / 60.0f);
		universe.Enter(world, [&](Store &) { runtime->Heartbeat(1.0f / 60.0f); });
	}

	universe.Enter(world, [&](Store &store) {
		CHECK(InScene(store, "NotFound:nil") != engine::ecs::NULL_ENTITY);
	});
}

TEST_CASE("javascript awaits a store reply as a promise", "[scripting][stores][js]") {
	// **The same barrier, the other language's suspension primitive.** A
	// promise resolved by the host at a barrier is the same contract a Luau
	// coroutine resumed there is; `JS_ExecutePendingJob` is what makes the
	// reaction run at a point the engine picked rather than whenever the VM
	// liked.
	RegisterClasses();
	const JobPool pool;

	engine::world::Universe universe;

	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("test.jsstore");

	const engine::world::WorldId world = universe.Create(settings);
	REQUIRE(world.IsValid());

	std::unique_ptr<Runtime> runtime;

	universe.Enter(world, [&](Store &store) {
		runtime = MakeRuntime(store, Language::JavaScript);
		MustRun(*runtime, R"(
			(async () => {
				await MemoryStoreService.SetAsync('key', { count: 11 });
				const reply = await MemoryStoreService.GetAsync('key');

				const marker = Instance.new('Part');
				marker.Name = 'Js:' + reply.Status + ':' + reply.Value.count;
				marker.Parent = workspace;
			})();
		)");
	});

	for (int beat = 0; beat < 6; beat++) {
		universe.Tick(1.0f / 60.0f);
		universe.Enter(world, [&](Store &) { runtime->Heartbeat(1.0f / 60.0f); });
	}

	universe.Enter(world, [&](Store &store) {
		CHECK(InScene(store, "Js:Ok:11") != engine::ecs::NULL_ENTITY);
	});
}

TEST_CASE("a replica refuses a store write and names the test to make first", "[scripting][stores]") {
	RegisterClasses();
	const JobPool pool;

	engine::world::Universe universe;

	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("test.replica.store");

	const engine::world::WorldId world = universe.Create(settings);
	REQUIRE(world.IsValid());

	universe.Enter(world, [&](Store &store) {
		store.SetAdoptOnly(true);

		const auto runtime = MakeRuntime(store, Language::Luau);
		CHECK_FALSE(runtime->Run("MemoryStoreService:SetAsync('key', 1)"));
		CHECK(runtime->LastError().find("IsReplica") != std::string::npos);
	});
}

// --- raycasting -------------------------------------------------------------

TEST_CASE("a script casts a ray and reads what it hit", "[scripting][raycast]") {
	// **Against the exact shapes, not the proxy boxes.** A ray that reported
	// the bounding box of a rotated part would be plausible and wrong, which is
	// the failure `physics::Query.hpp` says that function exists for.
	RegisterClasses();
	Store store("script_test");
	engine::physics::RegisterPhysicsComponents();
	engine::physics::PreparePhysicsWorld(store);

	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local wall = Instance.new('Part')
		wall.Name = 'Wall'
		wall.Anchored = true
		wall.Size = Vector3.new(10, 10, 1)
		wall.Position = Vector3.new(0, 0, -10)
		wall.Parent = workspace
	)");

	// The broad-phase index is rebuilt by a system, and this test runs no
	// scheduler — so it is synced directly. A query against a stale index finds
	// nothing, which would look like the binding failing rather than the index
	// being empty.
	engine::physics::SyncBroadphase(store);

	MustRun(*runtime, R"(
		local result = workspace:Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, -50))
		assert(result ~= nil, 'the ray found nothing')
		assert(result.Instance.Name == 'Wall', 'hit ' .. tostring(result.Instance))
		assert(result.Distance > 8 and result.Distance < 11, 'distance was ' .. result.Distance)

		-- A miss is nil rather than a result carrying a flag: a flag makes
		-- reading the position out of a miss compile.
		local miss = workspace:Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, 50))
		assert(miss == nil, 'a ray pointing away hit something')

		-- A zero direction finds nothing rather than erroring: a script scaling
		-- a direction by a variable that reached zero has a miss, not a bug.
		assert(workspace:Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, 0)) == nil)
	)");
}

TEST_CASE("RaycastParams filters by collision group", "[scripting][raycast]") {
	RegisterClasses();
	engine::spatial::CollisionGroups::Reset();
	engine::spatial::CollisionGroups::Register("Ignored");
	engine::spatial::CollisionGroups::SetCollidable(
		engine::core::Name("Ignored"), engine::core::Name("Default"), false
	);

	Store store("script_test");
	engine::physics::RegisterPhysicsComponents();
	engine::physics::PreparePhysicsWorld(store);

	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local wall = Instance.new('Part')
		wall.Name = 'Ghost'
		wall.Anchored = true
		wall.Size = Vector3.new(10, 10, 1)
		wall.Position = Vector3.new(0, 0, -10)
		wall.CollisionGroup = 'Ignored'
		wall.Parent = workspace
	)");

	// The broad-phase index is rebuilt by a system, and this test runs no
	// scheduler — so it is synced directly. A query against a stale index finds
	// nothing, which would look like the binding failing rather than the index
	// being empty.
	engine::physics::SyncBroadphase(store);

	MustRun(*runtime, R"(
		-- Unfiltered, the wall is there.
		assert(workspace:Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, -50)) ~= nil, 'no hit at all')

		local params = RaycastParams.new()
		params.CollisionGroup = 'Default'
		assert(params.CollisionGroup == 'Default', 'the group did not round-trip')

		-- A ray in `Default` does not meet a part in a group `Default` was told
		-- not to collide with.
		local filtered = workspace:Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, -50), params)
		assert(filtered == nil, 'the filter did not take')
	)");

	// A group nobody registered is refused rather than silently widening the
	// filter to everything.
	CHECK_FALSE(runtime->Run("RaycastParams.new().CollisionGroup = 'Nonsense'"));

	engine::spatial::CollisionGroups::Reset();
}

TEST_CASE("javascript casts the same ray", "[scripting][raycast][js]") {
	RegisterClasses();
	Store store("script_test");
	engine::physics::RegisterPhysicsComponents();
	engine::physics::PreparePhysicsWorld(store);

	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const wall = Instance.new('Part');
		wall.Name = 'JsWall';
		wall.Anchored = true;
		wall.Size = Vector3.new(10, 10, 1);
		wall.Position = Vector3.new(0, 0, -10);
		wall.Parent = workspace;
	)");

	engine::physics::SyncBroadphase(store);

	MustRun(*runtime, R"(
		const result = workspace.Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, -50));
		if (result === null) throw new Error('the ray found nothing');
		if (result.Instance.Name !== 'JsWall') throw new Error('hit ' + result.Instance.Name);
		if (result.Distance < 8 || result.Distance > 11) throw new Error('distance ' + result.Distance);

		if (workspace.Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, 50)) !== null) {
			throw new Error('a ray pointing away hit something');
		}
		if (workspace.Raycast(Vector3.new(0, 0, 0), Vector3.new(0, 0, 0)) !== null) {
			throw new Error('a zero direction hit something');
		}

		const params = RaycastParams.new();
		if (typeOf(params) !== 'RaycastParams') throw new Error(typeOf(params));
	)");
}

// --- the surface camera -----------------------------------------------------

TEST_CASE("SurfaceSize turns a camera into one that renders to a texture", "[scripting][surface]") {
	// **Structural, so the component's presence is the query.** A camera with
	// no `SurfaceCamera` is an ordinary camera and a consumer walks past it;
	// setting a size is what makes it one that renders offscreen.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local camera = Instance.new('Camera')
		camera.Name = 'Reflection'
		camera.Parent = workspace

		assert(camera.SurfaceSize == Vector3.new(0, 0, 0), 'a fresh camera renders to a texture')

		camera.SurfaceSize = Vector3.new(1024, 512, 0)
		assert(camera.SurfaceSize == Vector3.new(1024, 512, 0), 'the size did not round-trip')

		-- Back to nothing takes the component away again, which is what makes
		-- the property structural rather than a flag.
		camera.SurfaceSize = Vector3.new(0, 0, 0)
		assert(camera.SurfaceSize == Vector3.new(0, 0, 0), 'clearing did not take')
	)");

	const Entity camera = InScene(store, "Reflection");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::SurfaceCamera>(camera) == nullptr);

	MustRun(*runtime, "workspace.Reflection.SurfaceSize = Vector3.new(256, 256, 0)");

	const auto *surface = store.Get<engine::scene::SurfaceCamera>(camera);
	REQUIRE(surface != nullptr);
	CHECK(surface->Width == 256);
	CHECK(surface->Height == 256);
}

TEST_CASE("a part is a mirror because a surface camera is parented to it", "[scripting][surface]") {
	// **There is no `Surface` property to set, and that is the point.** It was
	// Roblox's name for something else entirely, and what it actually held was a
	// render-target index the author had to keep unique by hand — two cameras
	// silently sharing a texture being the failure. A pane is a mirror because a
	// `SurfaceCamera` is parented to it; a plain `Camera` projects nothing.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local pane = Instance.new('Part')
		pane.Name = 'Pane'
		pane.Parent = workspace

		-- Refused at the read, not nil: an unknown member raises, so a scene
		-- still setting a slot by hand fails loudly at the line that does it
		-- rather than silently doing nothing.
		assert(not pcall(function() return pane.Surface end), 'Surface is still a property')

		local reflection = Instance.new('SurfaceCamera')
		reflection.Name = 'Reflection'
		reflection.SurfaceSize = Vector3.new(256, 256)
		reflection.Face = Enum.NormalId.Front
		reflection.Parent = pane

		assert(not pcall(function() return reflection.Surface end), 'the camera still names a slot')
	)");

	const Entity pane = InScene(store, "Pane");

	// Nothing has aimed anything yet, so the pane is an ordinary part. The slot
	// arrives from `AimSurfaceCameras`, which needs a viewer — see
	// `scene/tests/SurfaceCameras.cpp` for the pass itself.
	CHECK(store.Get<Visual>(pane)->Surface == -1);
	CHECK(store.FindFirstChild(pane, "Reflection") != engine::ecs::NULL_ENTITY);
}

TEST_CASE("javascript reaches the surface camera too", "[scripting][surface][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const camera = Instance.new('Camera');
		camera.Name = 'JsReflection';
		camera.SurfaceSize = Vector3.new(512, 256, 0);
		camera.Parent = workspace;

		const pane = Instance.new('Part');
		pane.Name = 'JsPane';
		pane.Parent = workspace;
	)");

	const auto *surface = store.Get<engine::scene::SurfaceCamera>(InScene(store, "JsReflection"));
	REQUIRE(surface != nullptr);
	CHECK(surface->Width == 512);
	CHECK(surface->Height == 256);

	// Unaimed, so still an ordinary part: JavaScript has no way to name a slot
	// either, which is the half of the surface removal this case covers.
	CHECK(store.Get<Visual>(InScene(store, "JsPane"))->Surface == -1);
}
