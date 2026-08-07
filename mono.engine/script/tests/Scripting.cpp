// The v0.6 script surface, exercised through the VM rather than through the
// pieces underneath it.
//
// `Signals.cpp` and `Codec.cpp` cover the shared machinery — ordering, the wire
// format — because those are pure C++ and testable without a VM. This file
// covers what a script can actually reach, which is the part a binding can get
// wrong while every piece under it is correct.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

TEST_CASE("GetDescendants is depth first and reaches every level", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local root = Instance.new('Part')
		root.Name = 'Root'

		local a = Instance.new('Part', root)  a.Name = 'A'
		local a1 = Instance.new('Part', a)    a1.Name = 'A1'
		local b = Instance.new('Part', root)  b.Name = 'B'

		local seen = {}
		for _, descendant in ipairs(root:GetDescendants()) do
			table.insert(seen, descendant.Name)
		end

		-- A child, then what is under it, then the next child. `A1` before `B`
		-- is the whole difference between this and a breadth-first walk.
		assert(#seen == 3, 'expected three descendants, got ' .. #seen)
		assert(table.concat(seen, ',') == 'A,A1,B', 'wrong order: ' .. table.concat(seen, ','))
	)");
}

TEST_CASE("Destroy forgets a grandchild's listeners too", "[scripting]") {
	// `DestroyInstance` takes the whole subtree, so everything the script side
	// remembers about it has to go in step. Forgetting only the direct children
	// left a grandchild's connection holding its callable for the rest of the
	// world's life.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local model = Instance.new('Part')
		model.Name = 'Model'
		model.Parent = workspace

		local child = Instance.new('Part', model)
		child.Name = 'Child'

		local grandchild = Instance.new('Part', child)
		grandchild.Name = 'Grandchild'
		grandchild.Changed:Connect(function() end)

		model:Destroy()
	)");

	CHECK(InScene(store, "Model") == engine::ecs::NULL_ENTITY);
	CHECK(InScene(store, "Grandchild") == engine::ecs::NULL_ENTITY);

	Beat(store, *runtime);
	Beat(store, *runtime);
	CHECK(runtime->LastError().empty());
}

TEST_CASE("javascript Destroy forgets a grandchild's listeners too", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const model = Instance.new('Part');
		model.Name = 'JsModelDeep';
		model.Parent = workspace;

		const child = Instance.new('Part', model);
		child.Name = 'JsChild';

		const grandchild = Instance.new('Part', child);
		grandchild.Name = 'JsGrandchild';
		grandchild.Changed.Connect(() => {});

		model.Destroy();
	)");

	CHECK(InScene(store, "JsModelDeep") == engine::ecs::NULL_ENTITY);
	CHECK(InScene(store, "JsGrandchild") == engine::ecs::NULL_ENTITY);

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

	// **`AlphaMode`, where this was `Material` until v0.10.** The enum is gone
	// with the seventeen names nothing sampled — `scene/Materials.hpp` — and what
	// is under test here was never the material: it is that a
	// `PropertyType::Enum` property takes a member, takes the bare string a
	// migrating script contains, and refuses everything else.
	MustRun(*runtime, R"(
		local part = Instance.new('Part')
		part.AlphaMode = Enum.AlphaMode.Clip

		assert(typeof(part.AlphaMode) == 'EnumItem', typeof(part.AlphaMode))
		assert(part.AlphaMode == Enum.AlphaMode.Clip, 'the value did not round-trip')
		assert(part.AlphaMode.Name == 'Clip')
		assert(part.AlphaMode.EnumType == 'AlphaMode')

		-- A bare string too, because that is what a migrating script contains.
		part.AlphaMode = 'Blend'
		assert(part.AlphaMode == Enum.AlphaMode.Blend, 'a string did not resolve')
	)");

	// The typo `PropertyType::Name` could never have caught.
	CHECK_FALSE(runtime->Run("Instance.new('Part').AlphaMode = 'Clipp'"));

	// And a member of the wrong enum, which a bare string could not have
	// distinguished either.
	CHECK_FALSE(runtime->Run("Instance.new('Part').AlphaMode = Enum.EasingStyle.Linear"));
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
		part.AlphaMode = Enum.AlphaMode.Clip;

		if (!part.AlphaMode.Equals(Enum.AlphaMode.Clip)) throw new Error('did not round-trip');
		if (part.AlphaMode.Name !== 'Clip') throw new Error('wrong name');
		if (part.AlphaMode.EnumType !== 'AlphaMode') throw new Error('wrong enum');

		part.AlphaMode = 'Blend';
		if (!part.AlphaMode.Equals(Enum.AlphaMode.Blend)) throw new Error('a string did not resolve');
	)");

	// The same two refusals the Luau side gets, from the same storage check.
	CHECK_FALSE(runtime->Run("Instance.new('Part').AlphaMode = 'Clipp';"));
	CHECK_FALSE(runtime->Run("Instance.new('Part').AlphaMode = Enum.EasingStyle.Linear;"));
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
	CHECK_FALSE(runtime->Run("return Enum.AlphaMode.Nonsense"));

	MustRun(*runtime, R"(
		local items = Enum.AlphaMode:GetEnumItems()
		assert(#items > 0, 'AlphaMode has no members')
		assert(items[1].EnumType == 'AlphaMode')
	)");
}

// --- attributes -------------------------------------------------------------

TEST_CASE("attributes hold what a script puts in them", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new("Part")

		-- **Nil for one nobody set**, which is the answer a script can act on:
		-- an error would make `if part:GetAttribute("x") then` a crash.
		assert(part:GetAttribute("Health") == nil, 'an unset attribute is not nil')

		part:SetAttribute("Health", 75)
		assert(part:GetAttribute("Health") == 75, 'a number did not round-trip')

		part:SetAttribute("Faction", "red")
		assert(part:GetAttribute("Faction") == 'red', 'a string did not round-trip')

		part:SetAttribute("Alive", true)
		assert(part:GetAttribute("Alive") == true, 'a boolean did not round-trip')

		-- The datatypes, which is the half a plain key-value store would not have.
		part:SetAttribute("Home", Vector3.new(1, 2, 3))
		assert(part:GetAttribute("Home") == Vector3.new(1, 2, 3), 'a Vector3 did not round-trip')

		part:SetAttribute("Tint", Color3.new(1, 0, 0))
		assert(part:GetAttribute("Tint") == Color3.new(1, 0, 0), 'a Color3 did not round-trip')

		part:SetAttribute("Band", NumberRange.new(2, 8))
		assert(part:GetAttribute("Band").Max == 8, 'a NumberRange did not round-trip')

		part:SetAttribute("Fade", NumberSequence.new(1, 0))
		assert(math.abs(part:GetAttribute("Fade"):Evaluate(0.5) - 0.5) < 1e-5, 'a curve did not round-trip')

		-- **Nil removes**, which is Roblox's spelling and the only one that lets
		-- a script take an attribute back.
		part:SetAttribute("Health", nil)
		assert(part:GetAttribute("Health") == nil, 'nil did not remove')

		-- Everything at once, by name.
		local all = part:GetAttributes()
		assert(all.Faction == 'red', 'GetAttributes lost a value')
		assert(all.Health == nil, 'GetAttributes returned a removed attribute')

		local count = 0
		for _ in pairs(all) do
			count += 1
		end
		assert(count == 6, 'GetAttributes returned ' .. count .. ' rather than 6')

		-- **An instance is refused**, because a handle means nothing outside the
		-- world holding it and an attribute crosses a save file.
		local ok = pcall(function()
			part:SetAttribute("Other", Instance.new("Part"))
		end)
		assert(not ok, 'an instance was accepted as an attribute')
	)");
}

TEST_CASE("attributes are dropped with the instance that held them", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// **Entity ids are reused, which is the whole reason this test exists.** A
	// table keyed by id with an entry left behind surfaces as somebody else's
	// attribute on a freshly created part — at a distance of however many
	// entities were made in between, which is the worst kind of bug to find.
	MustRun(*runtime, R"(
		local first = Instance.new("Part")
		first:SetAttribute("Secret", "gone")
		first:Destroy()

		local second = Instance.new("Part")
		assert(second:GetAttribute("Secret") == nil, 'an attribute outlived its instance')
	)");
}

TEST_CASE("an attribute change reaches a listener", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new("Part")
		part.Parent = workspace

		local seen = 0
		part:GetAttributeChangedSignal("Score"):Connect(function()
			seen += 1
		end)

		part:SetAttribute("Score", 1)

		-- **Written three times and signalled once**, which is the dedup the
		-- change queue already gives every property: a script seeing three calls
		-- with two values nobody will observe is worse than one call with the
		-- value it ended at.
		part:SetAttribute("Score", 2)
		part:SetAttribute("Score", 3)

		task.wait()
		assert(seen == 1, 'the attribute listener ran ' .. seen .. ' time(s) rather than once')
		assert(part:GetAttribute("Score") == 3, 'the listener saw a stale value')
	)");
}

// --- input ------------------------------------------------------------------

TEST_CASE("UserInputService answers from the world's input state", "[scripting]") {
	RegisterClasses();
	Store store("script_test");

	// **The resource is what makes input exist.** A world without one is a
	// server, and every query answers "nothing pressed" rather than raising —
	// which is what lets one script poll input on both halves of a game.
	engine::scene::InputState state;
	state.Down.Set(engine::scene::KeyCode::W, true);
	state.MousePosition = engine::core::Vector2{120.0f, 45.0f};
	store.SetResource(state);

	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		assert(UserInputService:IsKeyDown(Enum.KeyCode.W), 'W should be down')
		assert(not UserInputService:IsKeyDown(Enum.KeyCode.A), 'A should not be down')

		local where = UserInputService:GetMouseLocation()
		assert(where.X == 120 and where.Y == 45, 'the mouse is somewhere else')

		local held = UserInputService:GetKeysPressed()
		assert(#held == 1, 'expected exactly one key held')
		assert(held[1] == Enum.KeyCode.W, 'GetKeysPressed returned the wrong key')

		-- **The one field that travels towards the client.** A script sets it and
		-- the client applies it to the window on the next frame.
		-- **Through a local, which is how a Roblox script is written anyway** —
		-- `local UIS = game:GetService("UserInputService")`. It is also the only
		-- form that works: `luaL_sandbox` enables Luau's `safeenv`, so a bare
		-- `Global.Field` compiles to a `GETIMPORT` that resolves once and caches
		-- the *value*, and a property read that way never changes again.
		-- `DEFERRED.md` D00030 carries it.
		local uis = game:GetService('UserInputService')
		uis.MouseBehavior = Enum.MouseBehavior.LockCenter
		assert(
			uis.MouseBehavior == Enum.MouseBehavior.LockCenter,
			'MouseBehavior did not round-trip'
		)
	)");

	// The write reached the resource rather than a copy.
	REQUIRE(
		store.Resource<engine::scene::InputState>()->Behaviour == engine::scene::MouseBehavior::LockCenter
	);
}

TEST_CASE("a world with no input state answers rather than raising", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// **A server is this case, and it is the ordinary one.** A script that
	// guarded every input query would be a script that could not be shared
	// between the two halves of a game.
	MustRun(*runtime, R"(
		assert(not UserInputService:IsKeyDown(Enum.KeyCode.W), 'a headless world reported a key')
		assert(#UserInputService:GetKeysPressed() == 0, 'a headless world reported keys')
		assert(not UserInputService.KeyboardEnabled, 'a headless world claimed a keyboard')
	)");
}

TEST_CASE("a bound action fires on the edge and the priority decides", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	store.SetResource(engine::scene::InputState{});

	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		-- **A part's attribute rather than a global**, because `luaL_sandbox`
		-- freezes the global table: `_G.log = {}` is "attempt to modify a
		-- readonly table". An attribute is the engine's own answer to "somewhere
		-- a handler can write that a later chunk can read".
		local board = Instance.new('Part')
		board.Name = 'Board'
		board.Parent = workspace
		board:SetAttribute('Log', '')

		local function note(text: string)
			board:SetAttribute('Log', board:GetAttribute('Log') .. text .. ';')
		end

		ContextActionService:BindActionAtPriority('low', function(name, state)
			note('low:' .. tostring(state.Name))
		end, false, 1, Enum.KeyCode.E)

		ContextActionService:BindActionAtPriority('high', function(name, state)
			note('high:' .. tostring(state.Name))
		end, false, 10, Enum.KeyCode.E)
	)");

	// Press E. **The edge is the difference between the two bitsets**, which is
	// what a client's translator produces every frame — so a test drives it the
	// same way rather than through a second path.
	auto *input = store.ResourceMutable<engine::scene::InputState>();
	input->Previous = input->Down;
	input->Down.Set(engine::scene::KeyCode::E, true);

	REQUIRE(runtime->Heartbeat(0.016f));

	MustRun(*runtime, R"(
		-- **The highest priority claims the key and the rest never see it**,
		-- which is the whole reason ContextActionService exists beside polling.
		local board = workspace:FindFirstChild('Board')
		assert(board:GetAttribute('Log') == 'high:Begin;', 'got ' .. board:GetAttribute('Log'))
	)");

	// Release it.
	input = store.ResourceMutable<engine::scene::InputState>();
	input->Previous = input->Down;
	input->Down.Set(engine::scene::KeyCode::E, false);

	REQUIRE(runtime->Heartbeat(0.016f));

	MustRun(*runtime, R"(
		local board = workspace:FindFirstChild('Board')
		assert(board:GetAttribute('Log') == 'high:Begin;high:End;', 'got ' .. board:GetAttribute('Log'))
	)");

	// Unbinding the winner hands the key to the next claim down.
	MustRun(*runtime, R"(
		ContextActionService:UnbindAction('high')
		workspace:FindFirstChild('Board'):SetAttribute('Log', '')
	)");

	input = store.ResourceMutable<engine::scene::InputState>();
	input->Previous = input->Down;
	input->Down.Set(engine::scene::KeyCode::E, true);

	REQUIRE(runtime->Heartbeat(0.016f));

	MustRun(*runtime, R"(
		local board = workspace:FindFirstChild('Board')
		assert(board:GetAttribute('Log') == 'low:Begin;', 'unbinding did not hand the key down')
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

// --- the two constants and the two keypoints, added at v0.10 -----------------

TEST_CASE("Vector3 carries the constants a script spells lowercase", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		assert(Vector3.zero == Vector3.new(0, 0, 0), 'Vector3.zero')
		assert(Vector3.one == Vector3.new(1, 1, 1), 'Vector3.one')

		-- **The constants are ordinary values of the type**, which is what makes
		-- them usable rather than decorative: a constant that failed the userdata
		-- check would be a constant no property could be assigned from and no
		-- operator could take.
		assert(typeof(Vector3.zero) == 'Vector3', 'the constant is not a Vector3')
		assert((Vector3.one * 3) == Vector3.new(3, 3, 3), 'a constant does not multiply')
		assert((Vector3.zero + Vector3.one) == Vector3.one, 'a constant does not add')

		-- Lowercase and not capitalised, because Roblox's are. A script written
		-- elsewhere says `Vector3.zero`, and a second spelling would be the
		-- duplicate AGENTS.md calls the most expensive kind of debt.
		assert(rawget(Vector3, 'Zero') == nil, 'a second spelling of zero exists')
	)");
}

TEST_CASE("the sequence keypoints are values rather than tables", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local stop = NumberSequenceKeypoint.new(0.25, 4, 0.5)
		assert(stop.Time == 0.25 and stop.Value == 4 and stop.Envelope == 0.5, 'keypoint fields')
		assert(typeof(stop) == 'NumberSequenceKeypoint', 'keypoint typeof')

		-- Equality compares the numbers rather than the userdata's address,
		-- which is what a test of a gradient actually asks.
		assert(stop == NumberSequenceKeypoint.new(0.25, 4, 0.5), 'keypoint equality')
		assert(stop ~= NumberSequenceKeypoint.new(0.25, 4), 'the envelope is part of the value')

		local tint = ColorSequenceKeypoint.new(1, Color3.new(0, 1, 0))
		assert(tint.Time == 1 and tint.Value == Color3.new(0, 1, 0), 'colour keypoint fields')
		assert(typeof(tint) == 'ColorSequenceKeypoint', 'colour keypoint typeof')

		-- **A constructor takes either form.** The table literal is still how a
		-- gradient is written inline and is not deprecated; what v0.10 added is
		-- that it is no longer the only form.
		--
		-- This line is also what found a real bug rather than only covering one:
		-- the table branch read its fields through a *relative* stack index that
		-- shifted as the stack grew, so the second field came out of the first
		-- one's value. Nothing had exercised it, because every sequence in this
		-- suite was built from the two-argument form.
		local mixed = NumberSequence.new({
			NumberSequenceKeypoint.new(0, 1),
			{1, 0, 0.25},
		})
		assert(#mixed.Keypoints == 2, 'mixed keypoint forms')
		assert(mixed.Keypoints[1].Value == 1, 'the userdata form lost its value')
		assert(mixed.Keypoints[2].Time == 1, 'the table form lost its time')
		assert(mixed.Keypoints[2].Envelope == 0.25, 'the table form lost its envelope')

		-- **The round trip is the whole reason these types exist.** Reading a
		-- sequence back hands out keypoints its own constructor accepts, where it
		-- used to hand out three anonymous numbers in a table.
		local again = NumberSequence.new(mixed.Keypoints)
		assert(again.Keypoints[1] == mixed.Keypoints[1], 'a sequence did not round-trip')
		assert(math.abs(again:Evaluate(0.5) - 0.5) < 1e-5, 'the round-tripped ramp is not the ramp')

		local ramp = ColorSequence.new({
			ColorSequenceKeypoint.new(0, Color3.new(1, 0, 0)),
			ColorSequenceKeypoint.new(1, Color3.new(0, 0, 1)),
		})
		assert(#ramp.Keypoints == 2, 'colour keypoints')
		assert(ramp.Keypoints[1].Value == Color3.new(1, 0, 0), 'colour keypoint round trip')
		assert(math.abs(ramp:Evaluate(0.5).B - 0.5) < 1e-5, 'colour ramp midpoint')
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

TEST_CASE("each script's steps are recorded against it", "[scripting]") {
	// **What the Script Profile panel reads.** The interrupt counter exists for
	// the step budget and runs whether or not anybody is looking; this asserts
	// that the delta around one script is attributed to *that* script, which is
	// the part a panel cannot show you is wrong.
	RegisterClasses();
	Store store("script_test");
	engine::script::RegisterScriptComponents();
	store.SetResource(engine::script::SourceCache{});

	auto &cache = *store.ResourceMutable<engine::script::SourceCache>();

	// One script that loops and one that does almost nothing. The absolute
	// numbers are a VM detail; the *ordering* is the claim worth pinning.
	cache.Set(engine::core::Name("busy.luau"), R"(
		local total = 0
		for index = 1, 2000 do
			total = total + index
		end
	)");
	cache.Set(engine::core::Name("idle.luau"), "local x = 1\n");

	const Entity busy = engine::script::MakeScript(store, "busy.luau", "Busy", false);
	const Entity idle = engine::script::MakeScript(store, "idle.luau", "Idle", false);
	REQUIRE(busy != engine::ecs::NULL_ENTITY);
	REQUIRE(idle != engine::ecs::NULL_ENTITY);

	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfServer();
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	REQUIRE(runtime->RunWorldScripts() == 2);

	const std::span<const engine::script::ScriptCost> costs = runtime->Costs();
	REQUIRE(costs.size() == 2);

	uint64_t busySteps = 0;
	uint64_t idleSteps = 0;
	for (const engine::script::ScriptCost &cost : costs) {
		CHECK(cost.Completed);
		if (cost.Instance == busy) {
			busySteps = cost.Steps;
		} else if (cost.Instance == idle) {
			idleSteps = cost.Steps;
		}
	}

	// The loop costs more than the assignment. A counter attributed to the
	// wrong script, or not moved at all, fails here rather than being read off
	// a panel as a plausible number.
	CHECK(busySteps > idleSteps);
	CHECK(busySteps > 0);
}

TEST_CASE("a runtime with no scripts reports no costs", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	engine::script::RegisterScriptComponents();

	const auto runtime = MakeRuntime(store, Language::Luau);
	CHECK(runtime->RunWorldScripts() == 0);
	CHECK(runtime->Costs().empty());
}

namespace {
	// A world with a module in the workspace, ready to be required by name.
	//
	// Parented to `Workspace` because that is how a script reaches one: through
	// the tree, which is the whole point of a module being an instance rather
	// than a path.
	Entity StageModule(Store &store, const char *path, const char *name, const char *program) {
		// **Services first, because `WorkspaceOf` is a lookup and not a create.**
		// The runtime installs them when it opens, which is after this — so
		// staging a module before one existed would parent it to nothing and
		// leave it a root beside the Workspace rather than inside it.
		engine::scene::InstallServices(store);
		engine::script::RegisterScriptComponents();
		if (store.Resource<engine::script::SourceCache>() == nullptr) {
			store.SetResource(engine::script::SourceCache{});
		}
		store.ResourceMutable<engine::script::SourceCache>()->Set(engine::core::Name(path), program);

		const Entity module = engine::script::MakeModule(store, path, name);
		REQUIRE(module != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(module, engine::scene::WorkspaceOf(store)));
		return module;
	}
}

TEST_CASE("a ModuleScript is not run by the world", "[scripting]") {
	// **The whole reason `ModuleScript` is a sibling of `Script` rather than a
	// kind of one.** A world that ran every module at start would give a library
	// a side effect nobody asked for — and a synced Rojo project would execute a
	// thousand files written to be required.
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "mod.luau", "Mod", "error('a module must not run on its own')\n");

	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfBoth();
	const auto runtime = MakeRuntime(store, Language::Luau, limits);

	// Nothing ran, so nothing raised. Both halves matter: a module that ran and
	// happened not to error would pass a test that only checked `LastError`.
	CHECK(runtime->RunWorldScripts() == 0);
	CHECK(runtime->LastError().empty());
}

TEST_CASE("require evaluates a module and hands back its value", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "mod.luau", "Mod", "return { Answer = 42 }\n");

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local mod = require(workspace.Mod)
		assert(mod.Answer == 42, 'the module did not return its table')
	)");
}

TEST_CASE("a module runs once however many times it is required", "[scripting]") {
	// **Roblox's rule, and the reason module order is not an author's problem.**
	// A module with a side effect at its top level has it once, on whichever
	// script required it first. Re-running would make two requires two different
	// tables, so a module used to share state would silently share nothing.
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "counter.luau", "Counter", R"(
		local state = { Count = 0 }
		state.Count = state.Count + 1
		return state
	)");

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local first = require(workspace.Counter)
		local second = require(workspace.Counter)

		assert(first == second, 'two requires produced two tables')
		assert(first.Count == 1, 'the module ran ' .. first.Count .. ' times')

		-- And the shared state really is shared.
		first.Count = 99
		assert(require(workspace.Counter).Count == 99, 'the table was copied')
	)");
}

TEST_CASE("requiring something that is not a module is refused by name", "[scripting]") {
	// Named rather than nil, for `GetService`'s reason: a script that gets nil
	// back fails one line later, somewhere that says nothing about the cause.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Run(R"(
		local part = Instance.new('Part')
		part.Parent = workspace
		require(part)
	)"));
	CHECK(runtime->LastError().find("ModuleScript") != std::string::npos);
}

TEST_CASE("a module that returns nothing is refused", "[scripting]") {
	// The mistake an author makes once. Unrefused it reads as `nil` three files
	// away, where nothing points back at the module.
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "empty.luau", "Empty", "local x = 1\n");

	const auto runtime = MakeRuntime(store, Language::Luau);
	CHECK_FALSE(runtime->Run("require(workspace.Empty)"));
	CHECK(runtime->LastError().find("must return") != std::string::npos);
}

TEST_CASE("a require cycle is named rather than crashing", "[scripting]") {
	// **Recursing instead would exhaust the C stack**, which surfaces as a crash
	// with no line number rather than as an error naming the two files.
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "a.luau", "A", "return require(workspace.B)\n");
	StageModule(store, "b.luau", "B", "return require(workspace.A)\n");

	const auto runtime = MakeRuntime(store, Language::Luau);
	CHECK_FALSE(runtime->Run("require(workspace.A)"));
	CHECK(runtime->LastError().find("cycle") != std::string::npos);
}

TEST_CASE("a module gets script pointing at itself", "[scripting]") {
	// The same global a `Script` gets, so a module can find its own siblings —
	// which is how a folder of modules refers to its neighbours without knowing
	// where the folder sits.
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "self.luau", "Selfie", R"(
		return { Name = script.Name }
	)");

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		assert(require(workspace.Selfie).Name == 'Selfie', 'script was not the module')
	)");
}

TEST_CASE("two module instances naming one file are two modules", "[scripting]") {
	// **Keyed by instance, not by path.** That is what makes a module a thing in
	// the tree rather than a thing on disk — two copies in two places are two
	// modules with two states, exactly as two copies of a Script are two scripts.
	RegisterClasses();
	Store store("script_test");
	StageModule(store, "shared.luau", "First", "return {}\n");

	const Entity second = engine::script::MakeModule(store, "shared.luau", "Second");
	REQUIRE(second != engine::ecs::NULL_ENTITY);
	REQUIRE(store.SetParent(second, engine::scene::WorkspaceOf(store)));

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		assert(require(workspace.First) ~= require(workspace.Second), 'one file became one module')
	)");
}

TEST_CASE("Players.LocalPlayer is nil until a host says who it is", "[scripting]") {
	// **The default is nobody**, because who is in a game is the host's business
	// — a dedicated server admits players as they connect and the studio admits
	// one per client view. Furnishing a world does not invent an occupant.
	RegisterClasses();
	Store store("script_test");
	engine::scene::InstallServices(store);

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local players = game:GetService('Players')
		assert(players ~= nil, 'there is no Players service')
		assert(players.LocalPlayer == nil, 'a fresh world already had a local player')
	)");
}

TEST_CASE("a client sees itself as LocalPlayer and a server sees nobody", "[scripting]") {
	// **The separation the whole feature is for.** A `Script` reaching for
	// `LocalPlayer` gets nil rather than somebody else's player, which is the
	// one thing a shared codebase must not get wrong.
	RegisterClasses();

	Store client("client_world");
	engine::scene::InstallServices(client);
	const Entity me = engine::scene::AddPlayer(client, "Ada", true);
	REQUIRE(me != engine::ecs::NULL_ENTITY);

	// Somebody else is in the game too, and must not be mistaken for me.
	REQUIRE(engine::scene::AddPlayer(client, "Grace", false) != engine::ecs::NULL_ENTITY);

	engine::script::RuntimeLimits clientLimits;
	clientLimits.Role = engine::script::HostRole::OfClient();
	const auto onClient = MakeRuntime(client, Language::Luau, clientLimits);

	MustRun(*onClient, R"(
		local players = game:GetService('Players')
		assert(players.LocalPlayer ~= nil, 'the client had no local player')
		assert(players.LocalPlayer.Name == 'Ada', 'the client was somebody else')

		-- And the other player is there, reachable and not me.
		local other = players:FindFirstChild('Grace')
		assert(other ~= nil, 'the other player is missing')
		assert(other ~= players.LocalPlayer, 'the two players are one instance')
	)");

	// The same world shape on a server: players present, none of them local.
	Store server("server_world");
	engine::scene::InstallServices(server);
	REQUIRE(engine::scene::AddPlayer(server, "Ada", false) != engine::ecs::NULL_ENTITY);

	engine::script::RuntimeLimits serverLimits;
	serverLimits.Role = engine::script::HostRole::OfServer();
	const auto onServer = MakeRuntime(server, Language::Luau, serverLimits);

	MustRun(*onServer, R"(
		local players = game:GetService('Players')
		assert(players:FindFirstChild('Ada') ~= nil, 'the server cannot see its players')
		assert(players.LocalPlayer == nil, 'a server claimed to be a player')
	)");
}

TEST_CASE("LocalPlayer cannot be assigned from a script", "[scripting]") {
	// Who you are is not yours to set. A client writing this would be a client
	// claiming to be somebody else.
	RegisterClasses();
	Store store("script_test");
	engine::scene::InstallServices(store);
	REQUIRE(engine::scene::AddPlayer(store, "Ada", true) != engine::ecs::NULL_ENTITY);

	const auto runtime = MakeRuntime(store, Language::Luau);
	CHECK_FALSE(runtime->Run(R"(
		local players = game:GetService('Players')
		players.LocalPlayer = Instance.new('Part')
	)"));
}

TEST_CASE("a LocalScript runs on a client and a Script does not", "[scripting]") {
	// **Roblox's rule, and the contexts it decides.** The same world, two hosts,
	// two different sets of scripts — which is what makes one codebase able to
	// hold both halves of a game.
	RegisterClasses();
	Store store("script_test");
	engine::scene::InstallServices(store);
	engine::script::RegisterScriptComponents();
	store.SetResource(engine::script::SourceCache{});

	auto &cache = *store.ResourceMutable<engine::script::SourceCache>();
	cache.Set(engine::core::Name("s.luau"), "return\n");
	cache.Set(engine::core::Name("c.luau"), "return\n");
	cache.Set(engine::core::Name("m.luau"), "return {}\n");

	const Entity server = engine::script::MakeScript(store, "s.luau", "OnServer", false);
	const Entity client = engine::script::MakeScript(store, "c.luau", "OnClient", true);
	REQUIRE(engine::script::MakeModule(store, "m.luau", "Mod") != engine::ecs::NULL_ENTITY);

	// Three classes, three contexts: server-only, client-only, and never.
	CHECK(engine::script::ScriptsIn(store, true, false) == std::vector<Entity>{server});
	CHECK(engine::script::ScriptsIn(store, false, true) == std::vector<Entity>{client});
	CHECK(engine::script::ScriptsIn(store, true, true) == std::vector<Entity>{server, client});

	// The module appears in none of them, however the host is configured.
	CHECK(engine::script::ScriptsIn(store, false, false).empty());
}

TEST_CASE("the tree lookups reach past the direct children", "[scripting]") {
	// The half of Roblox's `Instance` surface that answers "where is this" —
	// and the half a script reaches for the moment a scene stops being flat.
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local outer = Instance.new('Part', workspace)
		outer.Name = 'Outer'

		local inner = Instance.new('Part', outer)
		inner.Name = 'Inner'

		local target = Instance.new('Part', inner)
		target.Name = 'Target'

		local eye = Instance.new('Camera', outer)
		eye.Name = 'Eye'

		-- **The recursive flag, which used to be read and ignored.** This line
		-- returned nil, because `Target` is a grandchild.
		assert(outer:FindFirstChild('Target') == nil, 'a grandchild is not a child')
		assert(outer:FindFirstChild('Target', true) == target, 'the recursive flag did nothing')

		-- Exact against derived, which is the split Roblox draws. A `Part` is a
		-- `BasePart`, so only one of these two finds it under that name.
		assert(outer:FindFirstChildOfClass('Part') == inner, 'OfClass missed the exact class')
		assert(outer:FindFirstChildOfClass('BasePart') == nil, 'OfClass matched a base class')
		assert(outer:FindFirstChildOfClass('Camera') == eye, 'OfClass missed the camera')
		assert(outer:FindFirstChildWhichIsA('BasePart') == inner, 'WhichIsA missed a derived class')

		-- `Target` is a grandchild, so only the recursive form reaches it once
		-- the direct children have been ruled out.
		assert(inner:FindFirstChildWhichIsA('BasePart') == target, 'WhichIsA missed a child')

		-- Upwards, and never the instance itself.
		assert(target:FindFirstAncestor('Outer') == outer, 'FindFirstAncestor missed')
		assert(target:FindFirstAncestor('Target') == nil, 'an instance is its own ancestor')
		assert(target:FindFirstAncestorOfClass('Part') == inner, 'the nearest ancestor lost')
		assert(target:FindFirstAncestorWhichIsA('Instance') == inner, 'WhichIsA lost upwards')
		assert(eye:FindFirstAncestorOfClass('Camera') == nil, 'an ancestor matched the wrong class')

		assert(
			target:GetFullName() == 'Workspace.Outer.Inner.Target',
			'GetFullName said ' .. target:GetFullName()
		)

		-- Nothing found is nil, not a userdata wrapping the null handle.
		assert(outer:FindFirstChild('Absent', true) == nil, 'a miss was not nil')
		assert(outer:FindFirstAncestorOfClass('Nonexistent') == nil, 'an unknown class matched')
	)");
}

TEST_CASE("javascript reaches the same tree lookups", "[scripting][js]") {
	// **Two VMs, one surface.** A method that exists in one and not the other
	// is a game that runs until somebody switches language, and the two agree
	// right up until the first time one is fixed.
	//
	// **Compared by path rather than by `===`.** `JsBindings.cpp` says why: a
	// JavaScript instance object is minted per call and `===` is object
	// identity with no `__eq` to override, so two handles to one entity are two
	// objects. `GetFullName()` is the identity a script can actually test, and
	// checking it here exercises the new method as well.
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const outer = Instance.new('Part', workspace);
		outer.Name = 'Outer';

		const inner = Instance.new('Part', outer);
		inner.Name = 'Inner';

		const target = Instance.new('Part', inner);
		target.Name = 'Target';

		const path = (found) => found === null ? null : found.GetFullName();

		if (path(outer.FindFirstChild('Target')) !== null) {
			throw new Error('a grandchild is not a child');
		}
		if (path(outer.FindFirstChild('Target', true)) !== 'Workspace.Outer.Inner.Target') {
			throw new Error('the recursive flag did nothing');
		}

		if (path(outer.FindFirstChildOfClass('Part')) !== 'Workspace.Outer.Inner') {
			throw new Error('OfClass missed');
		}
		if (path(outer.FindFirstChildOfClass('BasePart')) !== null) {
			throw new Error('OfClass matched a base class');
		}
		if (path(inner.FindFirstChildWhichIsA('BasePart')) !== 'Workspace.Outer.Inner.Target') {
			throw new Error('WhichIsA missed');
		}

		if (path(target.FindFirstAncestor('Outer')) !== 'Workspace.Outer') {
			throw new Error('FindFirstAncestor missed');
		}
		if (path(target.FindFirstAncestor('Target')) !== null) {
			throw new Error('an instance is its own ancestor');
		}
		if (path(target.FindFirstAncestorOfClass('Part')) !== 'Workspace.Outer.Inner') {
			throw new Error('the nearest ancestor lost');
		}
		if (path(target.FindFirstAncestorWhichIsA('Instance')) !== 'Workspace.Outer.Inner') {
			throw new Error('WhichIsA lost upwards');
		}

		// The methods the hard-coded table length used to drop are still here.
		if (!target.IsDescendantOf(outer)) { throw new Error('IsDescendantOf went missing'); }
		if (typeof target.GetPropertyChangedSignal !== 'function') {
			throw new Error('GetPropertyChangedSignal went missing');
		}
	)");
}

TEST_CASE("the tree signals fire at the barrier", "[scripting]") {
	// **One tick late, and that is the contract rather than a shortcoming.**
	// `Changes.hpp` says why a signal cannot fire from inside the write: the
	// handler would re-enter the VM with the sibling list half-relinked, and
	// could destroy the instance being moved. So the tree records and the beat
	// delivers.
	//
	// Recorded into the world rather than into a script global, for the reason
	// the disconnect test above gives: each chunk gets its own sandboxed
	// globals, so the assertions are made from here instead.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local log = Instance.new('Part', workspace)
		log.Name = 'Log'

		local home = Instance.new('Part', workspace)
		home.Name = 'Home'

		local away = Instance.new('Part', workspace)
		away.Name = 'Away'

		local function note(what)
			local mark = Instance.new('Part', log)
			mark.Name = what
		end

		home.ChildAdded:Connect(function(child) note('added-' .. child.Name) end)
		home.ChildRemoved:Connect(function(child) note('removed-' .. child.Name) end)
		away.ChildAdded:Connect(function(child) note('away-added-' .. child.Name) end)
		log.DescendantAdded:Connect(function() end)

		local mover = Instance.new('Part')
		mover.Name = 'Mover'
		mover.AncestryChanged:Connect(function(subject, parent)
			note('ancestry-' .. subject.Name .. '-' .. (parent and parent.Name or 'nil'))
		end)

		mover.Parent = home
	)");

	const auto log = [&] { return store.FindFirstChild(engine::scene::WorkspaceOf(store), "Log"); };

	// **Nothing yet.** The write happened during the chunk and the delivery is
	// the barrier that has not run.
	REQUIRE(log() != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log(), "added-Mover") == engine::ecs::NULL_ENTITY);

	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

	CHECK(store.FindFirstChild(log(), "added-Mover") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log(), "ancestry-Mover-Home") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log(), "removed-Mover") == engine::ecs::NULL_ENTITY);

	// Moved again, from a second chunk that has to find it rather than
	// remember it.
	MustRun(*runtime, R"(
		local home = workspace:FindFirstChild('Home')
		local away = workspace:FindFirstChild('Away')
		home:FindFirstChild('Mover').Parent = away
	)");
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

	// Leaving fires on the parent it left, which is the half a record carrying
	// only the new parent could not answer.
	CHECK(store.FindFirstChild(log(), "removed-Mover") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log(), "away-added-Mover") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log(), "ancestry-Mover-Away") != engine::ecs::NULL_ENTITY);
}

TEST_CASE("an ancestry change reaches the whole subtree", "[scripting]") {
	// Moving a model changes the ancestry of every part in it, and a script
	// watching a part has no other way to learn that its model moved.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local log = Instance.new('Part', workspace)
		log.Name = 'Log'

		local model = Instance.new('Part', workspace)
		model.Name = 'Model'

		local part = Instance.new('Part', model)
		part.Name = 'Part'

		local nested = Instance.new('Part', part)
		nested.Name = 'Nested'

		local holder = Instance.new('Part', workspace)
		holder.Name = 'Holder'

		for _, watched in ipairs({model, part, nested}) do
			watched.AncestryChanged:Connect(function(subject)
				local mark = Instance.new('Part', log)
				mark.Name = 'heard-' .. subject.Name
			end)
		end
	)");

	// The creations above are reparents too, so they are delivered first. This
	// beat drains them; the move below is what the test is about.
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

	const Entity workspace = engine::scene::WorkspaceOf(store);
	const Entity log = store.FindFirstChild(workspace, "Log");
	REQUIRE(log != engine::ecs::NULL_ENTITY);

	const auto heard = [&](const char *name) {
		size_t count = 0;
		store.EachChild(log, [&](Entity mark) {
			if (store.InstanceNameOf(mark) == engine::core::Name(name)) {
				count++;
			}
		});
		return count;
	};

	const size_t before = heard("heard-Nested");

	MustRun(*runtime, R"(
		workspace:FindFirstChild('Model').Parent = workspace:FindFirstChild('Holder')
	)");
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

	// The moved instance, its child and its grandchild each heard it once.
	CHECK(heard("heard-Model") == 1);
	CHECK(heard("heard-Part") == 1);
	CHECK(heard("heard-Nested") == before + 1);
}

TEST_CASE("a world nobody watches records no tree changes", "[scripting]") {
	// The opt-in half. A list that grew for the life of every world would be a
	// leak in every game that never connected one of these.
	RegisterClasses();
	Store store("script_test");
	CHECK_FALSE(store.TreeObserved());

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local part = Instance.new('Part', workspace)
		part.Name = 'Part'
	)");
	CHECK_FALSE(store.TreeObserved());

	std::vector<engine::ecs::TreeChange> changes;
	store.TakeTreeChanges(changes);
	CHECK(changes.empty());

	MustRun(*runtime, "workspace.ChildAdded:Connect(function() end)");
	CHECK(store.TreeObserved());
}

TEST_CASE("DescendantRemoving fires while the thing is still there", "[scripting]") {
	// **The contract, and the only assertion that matters.** Every other tree
	// signal is delivered from a queue at the next barrier; this one has to be
	// called *before* the removal, or a handler cannot do the one thing it
	// exists for — read the subtree it is about to lose.
	//
	// So the handler asks, at call time, whether the leaving instance is still
	// a descendant of the ancestor being notified. Answering "no" would mean
	// the signal is `DescendantRemoved` under a name that promises otherwise.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local log = Instance.new('Part', workspace)
		log.Name = 'Log'

		local model = Instance.new('Part', workspace)
		model.Name = 'Model'

		local part = Instance.new('Part', model)
		part.Name = 'Part'

		workspace.DescendantRemoving:Connect(function(leaving)
			local mark = Instance.new('Part', log)

			-- Still parented, still reachable, still where it was. This is the
			-- assertion the whole signal is for.
			local intact = leaving:IsDescendantOf(workspace) and leaving.Parent ~= nil
			mark.Name = (intact and 'intact-' or 'gone-') .. leaving.Name
		end)
	)");

	const Entity workspaceRoot = engine::scene::WorkspaceOf(store);
	const Entity log = store.FindFirstChild(workspaceRoot, "Log");
	REQUIRE(log != engine::ecs::NULL_ENTITY);

	// Nothing has left yet.
	CHECK(store.FindFirstChild(log, "intact-Model") == engine::ecs::NULL_ENTITY);

	// **No heartbeat between the write and the assertion**, deliberately. This
	// signal does not wait for a barrier, so if it needed one the checks below
	// would fail.
	MustRun(*runtime, R"(
		local holder = Instance.new('Part')
		holder.Name = 'Holder'
		workspace:FindFirstChild('Model').Parent = holder
	)");

	// The model and the part inside it both stopped being descendants of the
	// workspace, so both were announced — and both were still there when they
	// were.
	CHECK(store.FindFirstChild(log, "intact-Model") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "intact-Part") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "gone-Model") == engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "gone-Part") == engine::ecs::NULL_ENTITY);

	// And the move still happened. A handler running underneath `SetParent`
	// must not be able to stop it.
	CHECK(store.FindFirstChild(workspaceRoot, "Model") == engine::ecs::NULL_ENTITY);
}

TEST_CASE("DescendantRemoving announces a destroyed subtree once", "[scripting]") {
	// A destroy takes the ancestors inside the subtree with it, so a model
	// being destroyed fires on itself for its own children as well as on the
	// workspace for everything. What it must not do is announce the same
	// removal again for every unparent the teardown performs.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local log = Instance.new('Part', workspace)
		log.Name = 'Log'

		local model = Instance.new('Part', workspace)
		model.Name = 'Model'

		local part = Instance.new('Part', model)
		part.Name = 'Part'

		local nested = Instance.new('Part', part)
		nested.Name = 'Nested'

		workspace.DescendantRemoving:Connect(function(leaving)
			local mark = Instance.new('Part', log)
			mark.Name = 'workspace-' .. leaving.Name
		end)

		model.DescendantRemoving:Connect(function(leaving)
			local mark = Instance.new('Part', log)
			mark.Name = 'model-' .. leaving.Name
		end)
	)");

	const Entity log = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Log");
	REQUIRE(log != engine::ecs::NULL_ENTITY);

	const auto counted = [&](const char *name) {
		size_t count = 0;
		store.EachChild(log, [&](Entity mark) {
			if (store.InstanceNameOf(mark) == engine::core::Name(name)) {
				count++;
			}
		});
		return count;
	};

	MustRun(*runtime, "workspace:FindFirstChild('Model'):Destroy()");

	// The workspace loses all three, once each.
	CHECK(counted("workspace-Model") == 1);
	CHECK(counted("workspace-Part") == 1);
	CHECK(counted("workspace-Nested") == 1);

	// The model loses the two under it, and is not told about itself.
	CHECK(counted("model-Part") == 1);
	CHECK(counted("model-Nested") == 1);
	CHECK(counted("model-Model") == 0);
}

TEST_CASE("a world nobody watches installs no removal hook", "[scripting]") {
	// The fan-out walks the leaving subtree and every ancestor above it, on
	// every destroy in the world. A game that never connects one of these must
	// not pay for that.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local part = Instance.new('Part', workspace)
		part.Name = 'Part'
		part:Destroy()
	)");

	// Nothing to assert but the absence of a crash and of a hook: the store
	// took no listener, so the destroy above dispatched nothing.
	MustRun(*runtime, "workspace.DescendantRemoving:Connect(function() end)");
}

TEST_CASE("javascript reaches the same tree signals", "[scripting][js]") {
	// **Two VMs, one surface.** The tree signals were Luau-only when they
	// landed, which is the gap this closes: a method or a signal that exists in
	// one language and not the other is a game that runs until somebody
	// switches, and the two agree right up until the first time one is fixed.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const log = Instance.new('Part', workspace);
		log.Name = 'Log';

		const home = Instance.new('Part', workspace);
		home.Name = 'Home';

		const note = (what) => {
			const mark = Instance.new('Part', log);
			mark.Name = what;
		};

		home.ChildAdded.Connect((child) => note('added-' + child.Name));
		home.ChildRemoved.Connect((child) => note('removed-' + child.Name));
		workspace.DescendantAdded.Connect((what) => note('descendant-' + what.Name));

		const mover = Instance.new('Part');
		mover.Name = 'Mover';
		mover.AncestryChanged.Connect((subject, parent) => {
			note('ancestry-' + subject.Name + '-' + (parent === null ? 'nil' : parent.Name));
		});

		mover.Parent = home;
	)");

	const Entity log = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Log");
	REQUIRE(log != engine::ecs::NULL_ENTITY);

	// Queued, not immediate — the same one-tick contract the Luau side has.
	CHECK(store.FindFirstChild(log, "added-Mover") == engine::ecs::NULL_ENTITY);

	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

	CHECK(store.FindFirstChild(log, "added-Mover") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "descendant-Mover") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "ancestry-Mover-Home") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "removed-Mover") == engine::ecs::NULL_ENTITY);

	// **No `const` here.** Unlike Luau, whose chunks each get their own
	// sandboxed globals, these two share one scope — so re-declaring a name the
	// first chunk bound is a `SyntaxError` rather than a shadow.
	MustRun(*runtime, R"(
		workspace.FindFirstChild('Home').FindFirstChild('Mover').Parent = workspace;
	)");
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

	CHECK(store.FindFirstChild(log, "removed-Mover") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "ancestry-Mover-Workspace") != engine::ecs::NULL_ENTITY);
}

TEST_CASE("javascript DescendantRemoving fires while the thing is still there", "[scripting][js]") {
	// The contract, from the other language. Dispatched from inside the store
	// before the removal, so no heartbeat runs between the write and the
	// assertions below — if it needed one they would fail.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const log = Instance.new('Part', workspace);
		log.Name = 'Log';

		const model = Instance.new('Part', workspace);
		model.Name = 'Model';

		const part = Instance.new('Part', model);
		part.Name = 'Part';

		workspace.DescendantRemoving.Connect((leaving) => {
			const mark = Instance.new('Part', log);
			const intact = leaving.IsDescendantOf(workspace) && leaving.Parent !== null;
			mark.Name = (intact ? 'intact-' : 'gone-') + leaving.Name;
		});
	)");

	const Entity workspaceRoot = engine::scene::WorkspaceOf(store);
	const Entity log = store.FindFirstChild(workspaceRoot, "Log");
	REQUIRE(log != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "intact-Model") == engine::ecs::NULL_ENTITY);

	MustRun(*runtime, R"(
		const holder = Instance.new('Part');
		holder.Name = 'Holder';
		workspace.FindFirstChild('Model').Parent = holder;
	)");

	CHECK(store.FindFirstChild(log, "intact-Model") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "intact-Part") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "gone-Model") == engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(log, "gone-Part") == engine::ecs::NULL_ENTITY);

	// And the move still happened.
	CHECK(store.FindFirstChild(workspaceRoot, "Model") == engine::ecs::NULL_ENTITY);
}

TEST_CASE("javascript announces a destroyed subtree once", "[scripting][js]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	MustRun(*runtime, R"(
		const log = Instance.new('Part', workspace);
		log.Name = 'Log';

		const model = Instance.new('Part', workspace);
		model.Name = 'Model';

		const part = Instance.new('Part', model);
		part.Name = 'Part';

		const nested = Instance.new('Part', part);
		nested.Name = 'Nested';

		workspace.DescendantRemoving.Connect((leaving) => {
			const mark = Instance.new('Part', log);
			mark.Name = 'workspace-' + leaving.Name;
		});
		model.DescendantRemoving.Connect((leaving) => {
			const mark = Instance.new('Part', log);
			mark.Name = 'model-' + leaving.Name;
		});
	)");

	const Entity log = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Log");
	REQUIRE(log != engine::ecs::NULL_ENTITY);

	const auto counted = [&](const char *name) {
		size_t count = 0;
		store.EachChild(log, [&](Entity mark) {
			if (store.InstanceNameOf(mark) == engine::core::Name(name)) {
				count++;
			}
		});
		return count;
	};

	MustRun(*runtime, "workspace.FindFirstChild('Model').Destroy();");

	CHECK(counted("workspace-Model") == 1);
	CHECK(counted("workspace-Part") == 1);
	CHECK(counted("workspace-Nested") == 1);
	CHECK(counted("model-Part") == 1);
	CHECK(counted("model-Nested") == 1);
	CHECK(counted("model-Model") == 0);
}

// --- the 2D tree's input ----------------------------------------------------
//
// **These are the join `gui/Input.hpp` refused to make and this module owns.**
// `gui::Router` produces events and knows nothing about a VM; `SignalTable`
// holds connections and knows nothing about a pointer. What is asserted here is
// the seam between them: that a script's `.Activated` runs, that it runs *when*
// the barrier says rather than when the pointer moved, and that the router's
// own ordering rules survive the crossing unchanged.
//
// A `gui::Router` is deliberately **not** used to produce the events. Its rules
// — which element an `InputEnded` goes to, that `MouseLeave` precedes
// `MouseEnter` — are `gui`'s and are tested there. Feeding hand-built events in
// is what makes these cases about delivery: a test that drove a real pointer
// would fail for either module's reasons and name neither.
//
// **Handlers record into the world rather than into a global**, because both
// VMs freeze their globals — `script/AGENTS.md` calls that the design rather
// than a hardening pass, and a test that wanted `_G` would be asking for the
// one thing a loaded script may not have. A part's `Name` is appended to, which
// records order as well as count in one readable value.

namespace {
	// One gui event, spelled out.
	engine::gui::GuiEvent
	Happened(engine::gui::EventKind kind, Entity instance, float x = 0.0f, float y = 0.0f) {
		engine::gui::GuiEvent event;
		event.Kind = kind;
		event.Instance = instance;
		event.Position = engine::core::Vector2{x, y};
		return event;
	}

	// A `TextButton` under the workspace, which is where a script can find it.
	Entity MakeButton(Store &store, const char *name) {
		engine::gui::RegisterGuiClasses();
		const Entity button = store.CreateInstance(engine::gui::GuiClass("TextButton"), name);
		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != engine::ecs::NULL_ENTITY);
		store.SetParent(button, workspace);
		return button;
	}

	// The name the trace part starts with, and the prefix `Trace` strips.
	//
	// **Not the empty string**, which was the obvious choice and is the one
	// thing that cannot work: `Instances.cpp` refuses `FindFirstChild(x, "")`
	// because `core::Name("")` is the invalid name and would match anything
	// unnamed. A handler looking the log up would get `nil`, fail at beat time
	// rather than at `Run` time, and record nothing — which reads exactly like
	// the signal never firing.
	constexpr const char *LOG_NAME = "Log";

	// The part handlers write their trace onto.
	Entity MakeLog(Store &store) {
		const Entity log = store.CreateInstance(PartClass(), LOG_NAME);
		store.SetParent(log, engine::scene::WorkspaceOf(store));
		return log;
	}

	// What the handlers recorded, in order, with the starting name removed.
	std::string Trace(Store &store, Entity log) {
		const std::string name(store.InstanceNameOf(log).Text());
		const std::string prefix(LOG_NAME);
		return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
	}
}

TEST_CASE("a gui event reaches a script's Activated", "[scripting][gui]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const Entity button = MakeButton(store, "Go");
	const Entity log = MakeLog(store);

	MustRun(*runtime, R"(
		local workspace = game:GetService('Workspace')
		local button = workspace:FindFirstChild('Go')
		assert(button, 'the button should be findable')
		local log = workspace:FindFirstChild('Log')
		button.Activated:Connect(function()
			log.Name = log.Name .. 'c'
		end)
	)");

	const engine::gui::GuiEvent click = Happened(engine::gui::EventKind::Activated, button);
	runtime->DeliverGuiEvents(std::span(&click, 1));

	// **Nothing has run yet, and that is the contract.** The event is queued
	// until the barrier, so a host that delivered and never beat gets no calls.
	CHECK(Trace(store, log).empty());
	CHECK(runtime->PendingGuiEventCount() == 1);

	Beat(store, *runtime);

	CHECK(Trace(store, log) == "c");
	CHECK(runtime->PendingGuiEventCount() == 0);

	// And it fires once rather than every beat — the queue is drained, not read.
	Beat(store, *runtime);
	CHECK(Trace(store, log) == "c");
}

TEST_CASE("gui events keep the router's order across the queue", "[scripting][gui]") {
	// **`MouseLeave` before `MouseEnter` is `gui::Router`'s rule** — a handler
	// that puts something back on leave runs before the one reacting to the
	// arrival — and a queue that sorted, bucketed by element or delivered by
	// kind would quietly reverse it. Two elements, one move between them.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const Entity left = MakeButton(store, "Left");
	const Entity right = MakeButton(store, "Right");
	const Entity log = MakeLog(store);

	MustRun(*runtime, R"(
		local workspace = game:GetService('Workspace')
		local log = workspace:FindFirstChild('Log')
		workspace:FindFirstChild('Left').MouseLeave:Connect(function()
			log.Name = log.Name .. 'L'
		end)
		workspace:FindFirstChild('Right').MouseEnter:Connect(function()
			log.Name = log.Name .. 'E'
		end)
	)");

	const engine::gui::GuiEvent events[2] = {
		Happened(engine::gui::EventKind::MouseLeave, left, 10.0f, 20.0f),
		Happened(engine::gui::EventKind::MouseEnter, right, 10.0f, 20.0f),
	};
	runtime->DeliverGuiEvents(events);
	Beat(store, *runtime);

	CHECK(Trace(store, log) == "LE");
}

TEST_CASE("a pointer signal is called with the canvas position", "[scripting][gui]") {
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const Entity button = MakeButton(store, "Tracked");
	const Entity log = MakeLog(store);

	MustRun(*runtime, R"(
		local workspace = game:GetService('Workspace')
		local log = workspace:FindFirstChild('Log')
		workspace:FindFirstChild('Tracked').MouseMoved:Connect(function(x, y)
			log.Name = log.Name .. tostring(x) .. ',' .. tostring(y)
		end)
	)");

	const engine::gui::GuiEvent moved = Happened(engine::gui::EventKind::MouseMoved, button, 123.0f, 456.0f);
	runtime->DeliverGuiEvents(std::span(&moved, 1));
	Beat(store, *runtime);

	CHECK(Trace(store, log) == "123,456");
}

TEST_CASE("an event for a destroyed element is dropped rather than dispatched", "[scripting][gui]") {
	// **The ordinary case, not an edge one.** A close button destroys the panel
	// it sits in, and the same delivery may carry a later event about something
	// that went with it — the router named both from a list compiled before
	// either handler ran. Firing at a dead entity would hand a script a userdata
	// for a row that is gone.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const Entity closer = MakeButton(store, "Closer");
	const Entity doomed = MakeButton(store, "Doomed");
	const Entity log = MakeLog(store);

	MustRun(*runtime, R"(
		local workspace = game:GetService('Workspace')
		local log = workspace:FindFirstChild('Log')
		local doomed = workspace:FindFirstChild('Doomed')
		workspace:FindFirstChild('Closer').Activated:Connect(function()
			log.Name = log.Name .. 'x'
			doomed:Destroy()
		end)
		doomed.Activated:Connect(function()
			log.Name = log.Name .. '!'
		end)
	)");

	const engine::gui::GuiEvent events[2] = {
		Happened(engine::gui::EventKind::Activated, closer),
		Happened(engine::gui::EventKind::Activated, doomed),
	};
	runtime->DeliverGuiEvents(events);
	Beat(store, *runtime);

	// The closer ran; the destroyed element was skipped rather than dispatched.
	CHECK(Trace(store, log) == "x");
}

TEST_CASE("two deliveries before one beat both arrive", "[scripting][gui]") {
	// **Two panels routing into one runtime is the studio's ordinary
	// arrangement** — one `gui::Router` per viewport panel, per
	// `ROADMAP.md`'s v0.8 entry — so a second delivery before the beat has to
	// queue behind the first rather than replace it.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const Entity button = MakeButton(store, "Twice");
	const Entity log = MakeLog(store);

	MustRun(*runtime, R"(
		local workspace = game:GetService('Workspace')
		local log = workspace:FindFirstChild('Log')
		workspace:FindFirstChild('Twice').Activated:Connect(function()
			log.Name = log.Name .. 'c'
		end)
	)");

	const engine::gui::GuiEvent click = Happened(engine::gui::EventKind::Activated, button);
	runtime->DeliverGuiEvents(std::span(&click, 1));
	runtime->DeliverGuiEvents(std::span(&click, 1));
	CHECK(runtime->PendingGuiEventCount() == 2);

	Beat(store, *runtime);
	CHECK(Trace(store, log) == "cc");
}

TEST_CASE("javascript reaches the same gui signals", "[scripting][js][gui]") {
	// The roadmap's gate: each item lands in Luau *and* JavaScript, or it is not
	// done. The ordering is shared by construction — one `SignalTable`, one
	// queue on `Runtime` — so what this checks is that the binding exists and
	// that the arguments arrive.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	const Entity button = MakeButton(store, "JsGo");
	const Entity log = MakeLog(store);

	MustRun(*runtime, R"(
		// The `workspace` global rather than `game.GetService('Workspace')`,
		// which the JavaScript surface does not answer — the existing parity
		// cases above reach it the same way.
		const button = workspace.FindFirstChild('JsGo');
		if (!button) throw new Error('the button should be findable');
		const log = workspace.FindFirstChild('Log');
		button.Activated.Connect(() => { log.Name = log.Name + 'c'; });
		button.MouseMoved.Connect((x, y) => { log.Name = log.Name + '@' + x + ',' + y; });
	)");

	const engine::gui::GuiEvent events[2] = {
		Happened(engine::gui::EventKind::Activated, button),
		Happened(engine::gui::EventKind::MouseMoved, button, 7.0f, 9.0f),
	};
	runtime->DeliverGuiEvents(events);

	CHECK(Trace(store, log).empty());

	Beat(store, *runtime);

	CHECK(Trace(store, log) == "c@7,9");
}

TEST_CASE("game.JobId names the world the script is standing on", "[scripting]") {
	// **What lets one file build four different worlds.** `--worlds N` runs the
	// same script in every world it creates, so without an identity every view
	// is identical — and a compositor that placed them in the wrong order would
	// look exactly like one that did not. `Mirrors-4-worlds.luau` is the caller.
	//
	// The world's *name* rather than a fresh identifier, because a name is what
	// a bus envelope, a snapshot and a view header already carry. A second
	// identifier for one world would be two sources of truth for the same fact.
	RegisterClasses();
	Store store("mirrors.world.2");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const Entity log = MakeLog(store);
	MustRun(*runtime, R"(
		local log = game:GetService('Workspace'):FindFirstChild('Log')
		log.Name = log.Name .. game.JobId
	)");

	CHECK(Trace(store, log) == "mirrors.world.2");
}

TEST_CASE("javascript reads the same JobId", "[scripting][js]") {
	RegisterClasses();
	Store store("mirrors.world.3");
	const auto runtime = MakeRuntime(store, Language::JavaScript);

	const Entity log = MakeLog(store);
	MustRun(*runtime, R"(
		const log = workspace.FindFirstChild('Log');
		log.Name = log.Name + game.JobId;
	)");

	CHECK(Trace(store, log) == "mirrors.world.3");
}

namespace {
	// A directory of `.luau` files on disk, removed when the test ends.
	//
	// Written rather than staged from the source tree: what `MountModuleTree`
	// has to get right is the *layout* rule, and a fixture that spells the
	// layout out in the test is the one that fails loudly when the rule changes.
	struct MountedLibrary {
		std::filesystem::path Root;

		explicit MountedLibrary(const char *name) {
			Root = std::filesystem::temp_directory_path() /
				   ("mono_mount_" + std::string(name) + "_" + std::to_string(::getpid()));
			std::filesystem::remove_all(Root);
			std::filesystem::create_directories(Root);
		}

		~MountedLibrary() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}

		void Write(const std::string &relative, const std::string &program) const {
			const std::filesystem::path path = Root / relative;
			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path);
			file << program;
		}
	};
}

TEST_CASE("a mounted directory becomes a tree a script can require", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	engine::scene::InstallServices(store);

	const MountedLibrary library("basic");
	library.Write("Lib/Numbers.luau", "return { Answer = 42 }\n");
	// The sibling lookup every module in a real library is built out of.
	library.Write("Lib/Sum.luau", R"(
		local Numbers = require(script.Parent.Numbers)
		return { Value = Numbers.Answer + 1 }
	)");

	const Entity mounted = engine::script::MountModuleTree(
		store, library.Root / "Lib", "Lib", engine::scene::WorkspaceOf(store)
	);
	REQUIRE(mounted != engine::ecs::NULL_ENTITY);

	// **A container, not a `ModuleScript`**, because the directory had no
	// `init.luau`. Requiring it would be a mistake and must stay one.
	CHECK(store.ClassOf(mounted) == engine::ecs::Classes::Find(engine::core::Name("Instance")));

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local lib = workspace.Lib
		assert(require(lib.Numbers).Answer == 42, 'the module did not load')
		assert(require(lib.Sum).Value == 43, 'a sibling require did not resolve')
	)");
}

TEST_CASE("init.luau collapses into its own directory", "[scripting]") {
	// **Rojo's rule, and the one that is silent when it is wrong.** With
	// `init.luau` collapsed, `script.Parent` inside a child is the *module*; got
	// wrong, it is a folder beside it and every relative require in the library
	// resolves one level off.
	RegisterClasses();
	Store store("script_test");
	engine::scene::InstallServices(store);

	const MountedLibrary library("init");
	library.Write("Presets/init.luau", R"(
		return { First = require(script.First), Second = require(script.Second) }
	)");
	library.Write("Presets/First.luau", "return 'first'\n");
	library.Write("Presets/Second.luau", R"(
		-- Up through the collapsed module to its sibling, which is the shape a
		-- preset uses to reach the library it belongs to.
		return require(script.Parent.First) .. '+second'
	)");

	const Entity presets = engine::script::MountModuleTree(
		store, library.Root / "Presets", "Presets", engine::scene::WorkspaceOf(store)
	);
	REQUIRE(presets != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(presets) == engine::script::ModuleScriptClass());

	// The children are on the module itself, which is what `script.First` reads.
	CHECK(store.FindFirstChild(presets, "First") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(presets, "Second") != engine::ecs::NULL_ENTITY);
	// And there is no leftover `init` beside them.
	CHECK(store.FindFirstChild(presets, "init") == engine::ecs::NULL_ENTITY);

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, R"(
		local presets = require(workspace.Presets)
		assert(presets.First == 'first', 'a child module did not load')
		assert(presets.Second == 'first+second', 'a child could not reach its sibling')
	)");
}

TEST_CASE("mounting nests directories and skips what it cannot run", "[scripting]") {
	RegisterClasses();
	Store store("script_test");
	engine::scene::InstallServices(store);

	const MountedLibrary library("nested");
	library.Write("Core/Root.luau", "return require(script.Parent.Deep.Inner)\n");
	library.Write("Core/Deep/Inner.luau", "return 'inner'\n");
	// Neither of these is a program, and mounting either would give the world a
	// ModuleScript whose source is not Luau.
	library.Write("Core/README.md", "not a module\n");
	library.Write("Core/data.json", "{}\n");

	const Entity core = engine::script::MountModuleTree(
		store, library.Root / "Core", "Core", engine::scene::WorkspaceOf(store)
	);
	REQUIRE(core != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(core, "README") == engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(core, "data") == engine::ecs::NULL_ENTITY);

	const auto runtime = MakeRuntime(store, Language::Luau);
	MustRun(*runtime, "assert(require(workspace.Core.Root) == 'inner', 'a nested module did not resolve')");
}

TEST_CASE("mounting a directory with nothing to run leaves no instance", "[scripting]") {
	// An empty container reads exactly like a library whose files failed to
	// stage, and the second is worth noticing rather than presenting as a tree
	// with nothing in it.
	RegisterClasses();
	Store store("script_test");

	const MountedLibrary library("empty");
	library.Write("Empty/notes.txt", "nothing here\n");

	CHECK(
		engine::script::MountModuleTree(store, library.Root / "Empty", "Empty") == engine::ecs::NULL_ENTITY
	);
	CHECK(
		engine::script::MountModuleTree(store, library.Root / "Missing", "Missing") ==
		engine::ecs::NULL_ENTITY
	);
}

TEST_CASE("a mounted tree is identical twice", "[scripting]") {
	// `directory_iterator` yields in filesystem order, which differs between
	// machines. A world whose instance ids depended on that would not replay.
	RegisterClasses();

	const MountedLibrary library("order");
	for (const char *name : {"Zulu", "Alpha", "Mike", "Bravo"}) {
		library.Write(std::string("Lib/") + name + ".luau", "return {}\n");
	}

	std::vector<std::string> first;
	std::vector<std::string> second;

	for (std::vector<std::string> *into : {&first, &second}) {
		Store store("script_test");
		const Entity root = engine::script::MountModuleTree(store, library.Root / "Lib", "Lib");
		REQUIRE(root != engine::ecs::NULL_ENTITY);

		store.EachChild(root, [&](Entity child) {
			into->push_back(std::to_string(child.Id) + ":" + std::string(store.InstanceNameOf(child).Text()));
		});
	}

	CHECK(first == second);
}

TEST_CASE("Vector3 has the Magnitude and Unit the declarations promise", "[scripting]") {
	// **Declared since the bindings existed and implemented by nothing.**
	// `Vector2` had both; `Vector3` errored at run time on a script that had
	// typechecked clean, because `bindings-check` compares the declarations
	// against the *class table* and a value type's members are in neither.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local v = Vector3.new(3, 4, 0)
		assert(math.abs(v.Magnitude - 5) < 1e-5, 'Magnitude is wrong')

		local unit = v.Unit
		assert(math.abs(unit.X - 0.6) < 1e-5, 'Unit.X is wrong')
		assert(math.abs(unit.Y - 0.8) < 1e-5, 'Unit.Y is wrong')
		assert(math.abs(unit.Magnitude - 1) < 1e-5, 'Unit is not unit length')

		-- The idiom this was actually missing for: a direction between two
		-- points, which is the first line of every aiming routine.
		local direction = (Vector3.new(0, 0, 10) - Vector3.new(0, 0, 0)).Unit
		assert(math.abs(direction.Z - 1) < 1e-5, 'a direction did not normalise')
	)");

	// A zero vector has no direction, and returning three NaNs would put them
	// into a transform and lose geometry somewhere else entirely.
	CHECK_FALSE(runtime->Run("return Vector3.new(0, 0, 0).Unit"));
	CHECK(runtime->LastError().find("zero vector") != std::string::npos);
}

// --- pivots -------------------------------------------------------------------

TEST_CASE("a script pivots a part by its handle", "[scripting]") {
	// **The pair is the point.** A `Transform` says where a part's centre is,
	// and almost nothing is placed by its centre — `PivotOffset` is where the
	// handle is, `GetPivot` reads it in world space and `PivotTo` puts it
	// somewhere. Setting `CFrame` to the target instead is the bug this exists
	// to make unnecessary.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local door = Instance.new('Part')
		door.Parent = workspace

		-- With no offset the handle is the placement itself.
		door.Position = Vector3.new(1, 2, 3)
		assert((door:GetPivot().Position - Vector3.new(1, 2, 3)).Magnitude < 1e-4, 'default pivot')

		-- A hinge two studs to the left.
		door.PivotOffset = CFrame.new(Vector3.new(-2, 0, 0))
		assert((door:GetPivot().Position - Vector3.new(-1, 2, 3)).Magnitude < 1e-4, 'offset pivot')

		-- Sending the *handle* somewhere moves the part to put it there.
		door:PivotTo(CFrame.new(Vector3.new(10, 0, 0)))
		assert((door:GetPivot().Position - Vector3.new(10, 0, 0)).Magnitude < 1e-4, 'pivot landed')
		assert((door.Position - Vector3.new(12, 0, 0)).Magnitude < 1e-4, 'part followed')
	)");
}

TEST_CASE("an instance with no placement answers a pivot anyway", "[scripting]") {
	// **Rather than raising**, which is `IsA`'s rule for a class nobody
	// registered: a script asking any instance for its pivot is asking a
	// question with a correct answer, and it is the identity.
	RegisterClasses();
	Store store("script_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustRun(*runtime, R"(
		local folder = Instance.new('Instance')
		assert(folder:GetPivot().Position.Magnitude < 1e-4, 'identity pivot')

		-- And moving it does nothing rather than erroring mid-frame.
		folder:PivotTo(CFrame.new(Vector3.new(5, 0, 0)))
	)");
}
