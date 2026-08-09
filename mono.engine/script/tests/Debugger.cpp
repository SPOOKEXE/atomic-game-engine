// Breakpoints, and what they caught.
//
// **The claim worth testing is that a breakpoint fires on the line somebody
// named and nowhere else.** A debugger that reports the wrong line is worse
// than none, because it sends the reader to a place that is working.
//
// The other half — that an unarmed runtime pays nothing — is structural rather
// than observable: `Run` switches single-step on only when `Armed()`, and the
// case below pins that a runtime with no breakpoints catches nothing.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.script.debugger")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Store;
using engine::script::BreakAction;
using engine::script::Debugger;
using engine::script::Language;
using engine::script::MakeRuntime;

namespace {
	// Five lines, so a breakpoint has somewhere unambiguous to sit. Line 1 is
	// empty because the raw string starts with a newline.
	constexpr const char *PROGRAM = R"(
local first = 1
local second = 2
local third = first + second
return third
)";
}

TEST_CASE("a breakpoint reports the line it was set on", "[debugger]") {
	Store store("debug_test");
	engine::scene::EnsureClassTree();
	engine::script::RegisterScriptComponents();
	store.SetResource(engine::script::SourceCache{});
	store.ResourceMutable<engine::script::SourceCache>()->Set(Name("probe.luau"), PROGRAM);

	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->Debug().Add("probe.luau", 4);

	REQUIRE(runtime->Debug().Armed());
	REQUIRE(runtime->Run(PROGRAM, "probe.luau"));

	const std::span<const engine::script::DebugHit> hits = runtime->Debug().Hits();
	REQUIRE_FALSE(hits.empty());

	// The line asked for, and no other. A capture on line 3 or 5 would send
	// somebody to code that is working.
	for (const engine::script::DebugHit &hit : hits) {
		CHECK(hit.Line == 4);
	}
}

TEST_CASE("a runtime with no breakpoints catches nothing", "[debugger]") {
	// **The free-when-unused property, as far as a test can see it.** Single
	// step is only switched on when something is armed, so an empty debugger
	// must produce an empty log rather than a filtered one.
	Store store("debug_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	CHECK_FALSE(runtime->Debug().Armed());
	REQUIRE(runtime->Run(PROGRAM, "probe.luau"));
	CHECK(runtime->Debug().Hits().empty());
}

TEST_CASE("a disabled breakpoint is kept and does not fire", "[debugger]") {
	// Kept rather than removed, so narrowing a problem down does not mean
	// retyping line numbers.
	Store store("debug_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	runtime->Debug().Add("probe.luau", 4);
	REQUIRE(runtime->Debug().Enable("probe.luau", 4, false));

	CHECK(runtime->Debug().Breakpoints().size() == 1);
	CHECK_FALSE(runtime->Debug().Armed());

	REQUIRE(runtime->Run(PROGRAM, "probe.luau"));
	CHECK(runtime->Debug().Hits().empty());
}

TEST_CASE("a stopping breakpoint ends the script as an ordinary error", "[debugger]") {
	// **An error rather than a new state.** The host already logs one, still
	// runs the remaining scripts and puts it in `LastError`, so stopping needs
	// no new path through the runtime.
	Store store("debug_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	runtime->Debug().Add("probe.luau", 4, BreakAction::Stop);

	CHECK_FALSE(runtime->Run(PROGRAM, "probe.luau"));
	CHECK(runtime->LastError().find("breakpoint") != std::string::npos);

	// And it still captured before it raised — a stop that reported nothing
	// would be a crash with extra steps.
	CHECK_FALSE(runtime->Debug().Hits().empty());
}

TEST_CASE("a capture names the locals in scope", "[debugger]") {
	Store store("debug_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Line 5 is `return third`, by which point all three locals exist.
	runtime->Debug().Add("probe.luau", 5);
	REQUIRE(runtime->Run(PROGRAM, "probe.luau"));

	const std::span<const engine::script::DebugHit> hits = runtime->Debug().Hits();
	REQUIRE_FALSE(hits.empty());
	REQUIRE_FALSE(hits.front().Frames.empty());

	bool foundThird = false;
	for (const engine::script::DebugLocal &local : hits.front().Frames.front().Locals) {
		if (local.Name == "third") {
			foundThird = true;
			// Rendered the way `print` would, so 3 reads as 3.
			CHECK(local.Value == "3");
		}
	}
	CHECK(foundThird);
}

TEST_CASE("the hit log is bounded", "[debugger]") {
	// A breakpoint inside a loop is the ordinary case, and an unbounded capture
	// log would fill memory faster than anybody could read one of them.
	Debugger debug;
	for (size_t index = 0; index < Debugger::MAXIMUM_HITS * 2; index++) {
		engine::script::DebugHit hit;
		hit.Line = static_cast<int>(index);
		debug.Record(std::move(hit));
	}

	REQUIRE(debug.Hits().size() == Debugger::MAXIMUM_HITS);

	// **The oldest went, not the newest.** Somebody watching a loop wants what
	// just happened, not what happened first.
	CHECK(debug.Hits().back().Line == static_cast<int>(Debugger::MAXIMUM_HITS * 2 - 1));
}

TEST_CASE("a breakpoint matches a path by its tail", "[debugger]") {
	// Luau reports a chunk by the name it was loaded under; a person types a
	// file name. Both have to work or the feature is unusable from the panel.
	Debugger debug;
	debug.Add("enemy.luau", 12);

	CHECK(debug.Match("scripts/ai/enemy.luau", 12) != nullptr);
	CHECK(debug.Match("scripts/ai/enemy.luau", 13) == nullptr);
	CHECK(debug.Match("scripts/ai/friend.luau", 12) == nullptr);
}

TEST_CASE("adding twice at one place replaces rather than duplicates", "[debugger]") {
	// Two breakpoints on one line is two reports of one event and no way to
	// tell which is which.
	Debugger debug;
	debug.Add("a.luau", 3, BreakAction::Capture);
	debug.Add("a.luau", 3, BreakAction::Stop);

	REQUIRE(debug.Breakpoints().size() == 1);
	CHECK(debug.Breakpoints().front().Action == BreakAction::Stop);
}

TEST_CASE("adopting takes the breakpoints and none of the hits", "[debugger]") {
	// **What survives a Stop.** The editor holds the master list and hands it to
	// each new runtime; a Stop destroys the runtime, and re-typing every line
	// number afterwards is how a debugger stops being used.
	Debugger master;
	master.Add("a.luau", 10, BreakAction::Stop);
	master.Add("b.luau", 20);
	REQUIRE(master.Enable("b.luau", 20, false));

	// A hit on the master, which nothing should carry across: it describes a
	// run that is over.
	engine::script::DebugHit stale;
	stale.Line = 10;
	master.Record(std::move(stale));

	Debugger fresh;
	fresh.Adopt(master);

	REQUIRE(fresh.Breakpoints().size() == 2);
	CHECK(fresh.Hits().empty());

	// The action and the enabled flag both came across — a copy that dropped
	// either would be a breakpoint that behaves differently after a Stop than
	// before it, which is worse than losing it outright.
	CHECK(fresh.Breakpoints()[0].Action == BreakAction::Stop);
	CHECK(fresh.Breakpoints()[0].Enabled);
	CHECK_FALSE(fresh.Breakpoints()[1].Enabled);

	// And the disabled one does not arm the fresh runtime on its own.
	CHECK(fresh.Armed());
	CHECK(fresh.Match("a.luau", 10) != nullptr);
	CHECK(fresh.Match("b.luau", 20) == nullptr);
}

TEST_CASE("adopting twice does not duplicate", "[debugger]") {
	// `BeginRun` may hand the list to a runtime that already has some — a
	// second Play without a Stop, or a world started while another runs.
	Debugger master;
	master.Add("a.luau", 10);

	Debugger fresh;
	fresh.Adopt(master);
	fresh.Adopt(master);

	CHECK(fresh.Breakpoints().size() == 1);
}
