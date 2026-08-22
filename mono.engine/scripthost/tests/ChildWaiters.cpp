// What `WaitForChild` waits for, what it gives up on, and what it refuses.
//
// **The suite exists because this is the one instance method that suspends.**
// Every other row in `NeutralInstanceMethods` answers from the store on the tick
// it was called, and `engine.script.scriptcall` can assert what it returned by
// running one chunk. A wait cannot be checked that way: what it answers with
// depends on what the world does over the *next* several beats, so every case
// here drives the barrier and looks at the world between beats.
//
// **The beat advances the world's tick, as the debris suite's does.** A timeout
// is a tick number - `ChildWaiters.hpp` says why seconds in and ticks underneath
// - so a heartbeat on a world whose clock never moves would test a deadline that
// never comes due.
//
// **The answer crosses as `workspace.Name`**, which is `engine.script.scriptcall`'s
// wire and is used here for its reason: `Runtime::Run` reports whether a chunk
// ran and not what it evaluated to, and a suspended chunk has not evaluated to
// anything yet in any case.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/ChildWaiters.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.scripthost.childwaiters")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::ChildWaiters;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	const std::vector<Language> LANGUAGES = {Language::Luau, Language::JavaScript};

	// What a half-second is at sixty hertz, which is the rate a fresh store
	// ticks at. Written out because the case asserting it is asserting exactly
	// this: the same script gives up on the same beat on every machine.
	constexpr int HALF_A_SECOND_IN_TICKS = 30;

	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	// One beat of the world, in `World::Tick`'s own order.
	void Beat(Store &store, Runtime &runtime) {
		store.ClearChanges();
		store.AdvanceTick(store.Time().Delta);
		REQUIRE(runtime.Heartbeat(store.Time().Delta));
		store.FlushSignals();
	}

	// A chunk that waits for a child of `workspace` and writes what it got.
	//
	// **Luau suspends the chunk itself and JavaScript suspends a function inside
	// it**, which is the one thing about a wait that a language genuinely
	// decides. `Runtime::Run` accepts a Luau chunk that yielded *because
	// something is scheduled to resume it*, so the top-level form is worth using
	// here rather than wrapping it in `task.spawn`: it asserts that path as well
	// as the wait. JavaScript has no top-level `await` under `JS_EVAL_TYPE_GLOBAL`,
	// so its half is the async form every promise-returning method here takes.
	std::string Waiter(Language language, std::string_view name, std::string_view timeout) {
		const std::string wanted(name);
		const std::string seconds(timeout);

		if (language == Language::Luau) {
			return "local child = workspace:WaitForChild('" + wanted + "', " + seconds +
				   ")\n"
				   "workspace.Name = if child then child.Name else 'none'\n";
		}
		return "(async () => {\n"
			   "	const child = await workspace.WaitForChild('" +
			   wanted + "', " + seconds +
			   ")\n"
			   "	workspace.Name = child === null ? 'none' : child.Name\n"
			   "})()\n";
	}

	// The one name the world reports its answer through.
	std::string Answer(const Store &store, Entity workspace) {
		return std::string(store.InstanceNameOf(workspace).Text());
	}

	// A child of `parent`, made by C++ rather than by a script.
	//
	// **Deliberately not made from a second chunk.** A waiter resumes on what the
	// *world* did, not on what a script did, and an engine system parenting
	// something has to wake a wait exactly as `Instance.new` does.
	Entity Arrives(Store &store, Entity parent, const char *name) {
		const Entity child = store.CreateInstance(engine::scene::PartClass(), name);
		REQUIRE(child != NULL_ENTITY);
		store.SetParent(child, parent);
		return child;
	}

	// Something to wait under, for the cases that drive the table directly.
	//
	// **An ordinary instance rather than the `Workspace`**, because the services
	// are installed by a *runtime* and these cases have none - `WorkspaceOf` on a
	// bare store answers a null entity, which is a parent that is never alive and
	// would make every wait below resume for the wrong reason.
	Entity Container(Store &store) {
		const Entity container = store.CreateInstance(engine::scene::PartClass(), "Box");
		REQUIRE(container != NULL_ENTITY);
		return container;
	}
}

TEST_CASE("a child that arrives later resumes the wait with it", "[scripting][waitforchild]") {
	// **The case the method exists for, and the one a lookup cannot answer.**
	// The child is absent when the script asks, absent for three more beats, and
	// then arrives - and the script that has been suspended the whole time is
	// handed the instance rather than nil.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("waitforchild_arrival");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != NULL_ENTITY);
		REQUIRE(store.SetInstanceName(workspace, "waiting"));

		INFO(runtime->LastError());
		REQUIRE(runtime->Run(Waiter(language, "late", "5").c_str()));

		// Still suspended, and the beats are what prove it: a version that
		// answered `FindFirstChild` immediately would have written 'none' here.
		for (int beat = 0; beat < 3; beat++) {
			Beat(store, *runtime);
		}
		CHECK(Answer(store, workspace) == "waiting");

		Arrives(store, workspace, "late");
		Beat(store, *runtime);

		INFO(runtime->LastError());
		CHECK(Answer(store, workspace) == "late");
	}
}

TEST_CASE("a child that is already there is answered without waiting", "[scripting][waitforchild]") {
	// **No beat at all between the call and the answer.** Roblox's first step,
	// and the reason it is worth asserting is that it is the half a wait must
	// *not* charge a tick for - a script reading a child that is plainly there
	// should not be a tick behind one that used `FindFirstChild`.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("waitforchild_present");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(store.SetInstanceName(workspace, "waiting"));
		Arrives(store, workspace, "here");

		INFO(runtime->LastError());
		REQUIRE(runtime->Run(Waiter(language, "here", "5").c_str()));
		CHECK(Answer(store, workspace) == "here");
	}
}

TEST_CASE("a wait gives up on the tick it was told to, twice the same", "[scripting][waitforchild]") {
	// **The deadline is a tick count, and this is what that buys.** Half a second
	// is thirty beats at sixty hertz on every machine and in every replay, so the
	// beat the answer lands on is asserted rather than a range - and the whole
	// scenario runs twice, because "the same script gave up at the same point
	// twice" is the property a wall-clock timeout would fail while still looking
	// approximately right.
	for (const Language language : LANGUAGES) {
		std::vector<int> landings;

		for (int run = 0; run < 2; run++) {
			Store store = Fresh("waitforchild_timeout");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			const Entity workspace = engine::scene::WorkspaceOf(store);
			REQUIRE(store.SetInstanceName(workspace, "waiting"));

			INFO(runtime->LastError());
			REQUIRE(runtime->Run(Waiter(language, "never", "0.5").c_str()));

			int landed = 0;
			for (int beat = 1; beat <= 40 && landed == 0; beat++) {
				Beat(store, *runtime);
				if (Answer(store, workspace) != "waiting") {
					landed = beat;
				}
			}

			CHECK(Answer(store, workspace) == "none");
			landings.push_back(landed);
		}

		REQUIRE(landings.size() == 2);
		CHECK(landings[0] == landings[1]);
		CHECK(landings[0] == HALF_A_SECOND_IN_TICKS);
	}
}

TEST_CASE("a timeout that is not a number of seconds gives up at once", "[scripting][waitforchild]") {
	// **`0/0` is one line of script away in either language**, and the deadline
	// it used to produce was a cast of NaN to an unsigned integer - undefined
	// behaviour rather than a long wait, because every comparison against NaN is
	// false and `TicksFor`'s guard was written as `<= 0`. It answers one tick for
	// anything that is not a positive number of seconds now, so the wait gives up
	// on the next beat rather than on whatever the conversion produced.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("waitforchild_nan");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(store.SetInstanceName(workspace, "waiting"));

		INFO(runtime->LastError());
		REQUIRE(runtime->Run(Waiter(language, "never", "0/0").c_str()));

		Beat(store, *runtime);
		CHECK(Answer(store, workspace) == "none");
	}
}

TEST_CASE("the form with no timeout is refused, and says why", "[scripting][waitforchild]") {
	// **Roblox's `WaitForChild(name)` waits for ever and this engine will not.**
	// The refusal is the divergence, so it is asserted as hard as the behaviour:
	// the call fails, in both languages, with a message naming the argument that
	// is missing - because an author porting a place meets this message and
	// nothing else.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("waitforchild_unbounded");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const std::string source = language == Language::Luau ? "workspace:WaitForChild('nope')\n"
															  : "workspace.WaitForChild('nope')\n";

		INFO(source);
		CHECK_FALSE(runtime->Run(source.c_str()));

		const std::string failure = runtime->LastError();
		INFO(failure);
		CHECK(failure.find("timeout") != std::string::npos);
	}
}

TEST_CASE("two waits for one child are answered in the order they were made", "[scripting][waitforchild]") {
	// **The ordering a recording depends on**, which is why the table is shared
	// machinery rather than one per VM: two scripts waiting for the same child
	// resume in the order they asked, on every run.
	Store store = Fresh("waitforchild_order");
	const Entity container = Container(store);

	ChildWaiters waiters;
	const uint64_t first = waiters.Add(container, "twin", store.Time().Tick + 100);
	const uint64_t second = waiters.Add(container, "twin", store.Time().Tick + 100);
	REQUIRE(first != 0);
	REQUIRE(second != 0);

	const Entity child = Arrives(store, container, "twin");

	std::vector<ChildWaiters::Resumption> ready;
	waiters.Advance(store, store.Time().Tick, ready);

	REQUIRE(ready.size() == 2);
	CHECK(ready[0].Waiter == first);
	CHECK(ready[1].Waiter == second);
	CHECK(ready[0].Child == child);
	CHECK(ready[1].Child == child);

	// Answered exactly once: a second pass over an emptied table finds nothing
	// even though the child is still there.
	ready.clear();
	waiters.Advance(store, store.Time().Tick, ready);
	CHECK(ready.empty());
	CHECK(waiters.Empty());
}

TEST_CASE("a child renamed into the name answers a wait too", "[scripting][waitforchild]") {
	// **The reason the match is a store lookup rather than a filter over
	// `ecs::TreeChange`.** Renaming a child that is already parented produces no
	// reparent at all, so an arrival-list filter would leave this script waiting
	// until its timeout - for a child that is, by then, plainly there.
	Store store = Fresh("waitforchild_rename");
	const Entity container = Container(store);
	const Entity child = Arrives(store, container, "before");

	ChildWaiters waiters;
	const uint64_t waiter = waiters.Add(container, "after", store.Time().Tick + 100);

	std::vector<ChildWaiters::Resumption> ready;
	waiters.Advance(store, store.Time().Tick, ready);
	CHECK(ready.empty());

	REQUIRE(store.SetInstanceName(child, "after"));
	waiters.Advance(store, store.Time().Tick, ready);

	REQUIRE(ready.size() == 1);
	CHECK(ready[0].Waiter == waiter);
	CHECK(ready[0].Child == child);
}

TEST_CASE("a wait whose parent is destroyed is answered at once", "[scripting][waitforchild]") {
	// **Nothing can ever arrive under a row that has gone**, so the deadline is
	// not worth waiting out - and a script left suspended on one is holding a
	// coroutine for a question that has already been answered.
	Store store = Fresh("waitforchild_dead");
	const Entity box = Container(store);

	ChildWaiters waiters;
	const uint64_t waiter = waiters.Add(box, "never", store.Time().Tick + 1000);
	store.DestroyInstance(box);

	std::vector<ChildWaiters::Resumption> ready;
	waiters.Advance(store, store.Time().Tick, ready);

	REQUIRE(ready.size() == 1);
	CHECK(ready[0].Waiter == waiter);
	CHECK(ready[0].Child == NULL_ENTITY);
}

TEST_CASE("a full queue refuses a wait rather than dropping one", "[scripting][waitforchild]") {
	// **The opposite trade from `DebrisQueue`, and the direction is the point.**
	// Evicting the oldest waiter would resume a script with nil for a child that
	// was about to arrive - a wrong answer in the one case the method exists for
	// - where refusing is an error naming the script that filled the queue.
	Store store = Fresh("waitforchild_full");
	const Entity container = Container(store);

	ChildWaiters waiters;
	for (size_t made = 0; made < ChildWaiters::MAXIMUM; made++) {
		REQUIRE(waiters.Add(container, "never", store.Time().Tick + 1000) != 0);
	}

	CHECK(waiters.Add(container, "never", store.Time().Tick + 1000) == 0);
	CHECK(waiters.Count() == ChildWaiters::MAXIMUM);
}
