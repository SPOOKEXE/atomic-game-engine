// When `Debris` destroys something, and what it does when a script fills it.
//
// **The beat here advances the world's tick, unlike the tween suite's.** A
// deadline is a tick number - `Debris.hpp` says why seconds in and ticks
// underneath - so a heartbeat on a world whose clock never moves would test a
// queue that never comes due. `Scripting.cpp`'s `Beat` is the shape, because it
// is the order `World::Tick` actually runs in.

#include "../src/Debris.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.script.debris")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::DebrisQueue;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	const std::vector<Language> LANGUAGES = {Language::Luau, Language::JavaScript};

	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	// One beat of the world, in `World::Tick`'s own order - `Scripting.cpp`'s
	// helper, and the tick advance is the part this suite cannot do without.
	void Beat(Store &store, Runtime &runtime) {
		store.ClearChanges();
		store.AdvanceTick(store.Time().Delta);
		REQUIRE(runtime.Heartbeat(store.Time().Delta));
		store.FlushSignals();
	}

	// `a:b(...)` in Luau and `a.b(...)` in JavaScript.
	std::string Send(Language language, std::string_view receiver, std::string_view call) {
		return std::string(receiver) + (language == Language::Luau ? ":" : ".") + std::string(call) + "\n";
	}

	// Whether the world still holds a part by that name, as a string both
	// languages spell the same way.
	std::string StillThere(Language language) {
		const std::string colon = language == Language::Luau ? ":" : ".";
		const std::string text = language == Language::Luau ? "tostring(" : "String(";
		const std::string nothing = language == Language::Luau ? " ~= nil" : " !== null";
		return "workspace.Name = " + text + "workspace" + colon + "FindFirstChild('doomed')" + nothing +
			   ")\n";
	}
}

TEST_CASE("an item is destroyed on its beat and not before", "[scripting][debris]") {
	// **Thirty ticks for half a second at sixty hertz, on every machine.** That
	// is the whole reason a lifetime is rounded to ticks rather than counted in
	// seconds against a clock: this case would otherwise pass or fail on how
	// busy the machine running it was.
	//
	// The beat *before* is asserted as hard as the beat itself, because a queue
	// that fired early and a queue that fired late are both wrong and only one
	// of them is obvious.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("debris_beat");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != NULL_ENTITY);

		const std::string source = (language == Language::Luau ? "local " : "let ") +
								   std::string("part = Instance.new('Part')\n") + "part.Name = 'doomed'\n" +
								   "part.Parent = workspace\n" +
								   Send(language, "Debris", "AddItem(part, 0.5)");

		INFO(source);
		REQUIRE(runtime->Run(source.c_str()));

		for (int beat = 0; beat < 29; beat++) {
			Beat(store, *runtime);
		}

		INFO("twenty-nine beats");
		REQUIRE(runtime->Run(StillThere(language).c_str()));
		CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "true");

		// The chunk above renamed the Workspace, which is how the answer
		// crosses. Put it back so the lookup below is the same lookup.
		REQUIRE(runtime->Run("workspace.Name = 'Workspace'"));

		Beat(store, *runtime);

		INFO("thirty beats");
		REQUIRE(runtime->Run(StillThere(language).c_str()));
		CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "false");
	}
}

TEST_CASE("an item destroyed before its beat is not a problem", "[scripting][debris]") {
	// **Ordinary, and the queue must neither crash nor hold the slot.** A script
	// that queues something and then destroys it itself is what a cleanup path
	// looks like when two of them run - and `DestroyInstance` on a row that has
	// gone is already a no-op, which is why there is no second removal path for
	// this to get wrong.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("debris_early");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const std::string source =
			(language == Language::Luau ? "local " : "let ") + std::string("part = Instance.new('Part')\n") +
			"part.Parent = workspace\n" + Send(language, "Debris", "AddItem(part, 0.5)") +
			Send(language, "part", "Destroy()");

		INFO(source);
		REQUIRE(runtime->Run(source.c_str()));

		// Well past the deadline. Every beat is required to succeed, which is
		// the assertion: a destroy of a dead row would surface here.
		for (int beat = 0; beat < 40; beat++) {
			Beat(store, *runtime);
		}
	}
}

TEST_CASE("a lifetime is rounded up and never to zero", "[scripting][debris]") {
	// **`task.wait`'s rule, settled the same way rather than differently.** A
	// zero lifetime destroys on the next beat rather than inside the call -
	// where the instance would go while the script that named it is still
	// running, and `part.Parent` on the next line would be reading a row that
	// has gone.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("debris_zero");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const Entity workspace = engine::scene::WorkspaceOf(store);
		const std::string source = (language == Language::Luau ? "local " : "let ") +
								   std::string("part = Instance.new('Part')\n") + "part.Name = 'doomed'\n" +
								   "part.Parent = workspace\n" +
								   Send(language, "Debris", "AddItem(part, 0)") + StillThere(language);

		INFO(source);
		REQUIRE(runtime->Run(source.c_str()));
		CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "true");

		REQUIRE(runtime->Run("workspace.Name = 'Workspace'"));
		Beat(store, *runtime);

		REQUIRE(runtime->Run(StillThere(language).c_str()));
		CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "false");
	}
}

// --- the queue underneath ----------------------------------------------------

TEST_CASE("the queue drains in deadline order, then insertion order", "[scripting][debris]") {
	// **Ties break on insertion, which is what makes two items with one
	// deadline go in the order the script asked.** A heap ordered on the tick
	// alone would leave that to whichever way the comparison happened to fall,
	// and a recording would then depend on it.
	Store store("debris_order");

	const Entity first = store.Create();
	const Entity second = store.Create();
	const Entity early = store.Create();

	DebrisQueue queue;
	CHECK(queue.Add(first, 10) == NULL_ENTITY);
	CHECK(queue.Add(second, 10) == NULL_ENTITY);
	CHECK(queue.Add(early, 5) == NULL_ENTITY);

	std::vector<Entity> expired;
	queue.Advance(4, expired);
	CHECK(expired.empty());

	queue.Advance(5, expired);
	REQUIRE(expired.size() == 1);
	CHECK(expired.front() == early);

	expired.clear();
	queue.Advance(10, expired);
	REQUIRE(expired.size() == 2);
	CHECK(expired[0] == first);
	CHECK(expired[1] == second);
	CHECK(queue.Count() == 0);
}

TEST_CASE("adding one instance twice keeps the earlier deadline", "[scripting][debris]") {
	// **One entry, not two.** A script counting something down in a loop would
	// otherwise fill the queue with one instance - and the second destruction of
	// an entity is a no-op, so the extra entries were only ever dead weight.
	Store store("debris_twice");
	const Entity part = store.Create();

	DebrisQueue queue;
	CHECK(queue.Add(part, 100) == NULL_ENTITY);
	CHECK(queue.Add(part, 20) == NULL_ENTITY);
	CHECK(queue.Count() == 1);

	std::vector<Entity> expired;
	queue.Advance(20, expired);
	REQUIRE(expired.size() == 1);
	CHECK(expired.front() == part);

	// And the later deadline does not survive the earlier one.
	expired.clear();
	queue.Advance(100, expired);
	CHECK(expired.empty());
}

TEST_CASE("a full queue destroys its oldest item early", "[scripting][debris]") {
	// **The cap fails by tidying up sooner, which is the conservative direction
	// for a cleanup call to be wrong in.** The evicted item was going to be
	// destroyed anyway; the only cost is that it went before its deadline, where
	// a refusal would leave a script's rubbish in the world forever.
	Store store("debris_cap");

	DebrisQueue queue;
	std::vector<Entity> added;
	added.reserve(DebrisQueue::MAXIMUM);

	for (size_t item = 0; item < DebrisQueue::MAXIMUM; item++) {
		const Entity instance = store.Create();
		added.push_back(instance);
		REQUIRE(queue.Add(instance, 1000) == NULL_ENTITY);
	}

	CHECK(queue.Count() == DebrisQueue::MAXIMUM);

	const Entity late = store.Create();
	CHECK(queue.Add(late, 1000) == added.front());
	CHECK(queue.Count() == DebrisQueue::MAXIMUM);

	// The evicted one is gone from the queue as well as handed back, so it
	// cannot be destroyed twice.
	std::vector<Entity> expired;
	queue.Advance(1000, expired);
	REQUIRE(expired.size() == DebrisQueue::MAXIMUM);
	CHECK(expired.front() == added[1]);
	CHECK(expired.back() == late);
}

TEST_CASE("a debris deadline and a wait of the same length come due together", "[scripting][debris]") {
	// **One arithmetic, asserted from both sides.** `script/AGENTS.md` states
	// that a debris deadline is a tick number "computed by the same
	// `ceil(seconds / delta)` `task.wait` uses" - and until v0.18 that sentence
	// was true only because three files happened to contain the same four lines.
	// `TicksFor` had a copy in `DebrisService.cpp`, one in `LuauTask.cpp` and one
	// in `JsSurface.cpp`, each taking a different handle to the same world, so
	// the claim was a convention nothing checked.
	//
	// It is one function in `Tasks.hpp` now, and this is what would notice if it
	// stopped being one: both facts are asserted on *both* beats rather than
	// either alone, because one of the two firing a tick early is exactly what a
	// second copy of the rounding would produce.
	//
	// **A duration of one and a half ticks, so rounding is what decides.** A
	// whole number of ticks would pass against `floor`, `ceil` and a truncation
	// alike.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("debris_wait_agree");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != NULL_ENTITY);

		const double seconds = static_cast<double>(store.Time().Delta) * 1.5;
		const std::string wait = "task.wait(" + std::to_string(seconds) + ")";

		// **The waiter parents a part rather than setting a global**, because
		// each chunk's globals are its own - and rather than writing the
		// workspace's name, which is what the rest of this suite reads its
		// answer out of and which the two would then be racing for.
		const std::string mark =
			language == Language::Luau
				? "local m = Instance.new('Part')\n  m.Name = 'woke'\n  m.Parent = workspace\n"
				: "const m = Instance.new('Part'); m.Name = 'woke'; m.Parent = workspace;";

		const std::string body = language == Language::Luau
									 ? "task.spawn(function()\n  " + wait + "\n  " + mark + "end)\n"
									 : "task.spawn(async () => { await " + wait + "; " + mark + " })\n";

		const std::string source =
			(language == Language::Luau ? "local " : "let ") + std::string("part = Instance.new('Part')\n") +
			"part.Name = 'doomed'\n" + "part.Parent = workspace\n" +
			Send(language, "Debris", "AddItem(part, " + std::to_string(seconds) + ")") + body;

		INFO(source);
		REQUIRE(runtime->Run(source.c_str()));

		// One beat short, and neither has happened.
		Beat(store, *runtime);
		CHECK(store.FindFirstChild(workspace, "doomed") != NULL_ENTITY);
		CHECK(store.FindFirstChild(workspace, "woke") == NULL_ENTITY);

		// The beat both are due on.
		Beat(store, *runtime);
		CHECK(store.FindFirstChild(workspace, "doomed") == NULL_ENTITY);
		CHECK(store.FindFirstChild(workspace, "woke") != NULL_ENTITY);
	}
}
