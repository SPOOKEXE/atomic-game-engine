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

#include <memory>
#include <span>
#include <string>
#include <vector>

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

// --- upvalues ------------------------------------------------------------------

namespace {
	// A closure over a local, which is what makes an upvalue exist at all.
	//
	// `counter` is a local of the *chunk* and an upvalue of `bump`, so the two
	// lists have to disagree about it — which is the whole property under test.
	constexpr const char *CLOSURE = R"(
local counter = 0
local label = "tally"
local function bump(step)
    counter = counter + step
    return counter
end
bump(5)
return counter
)";
}

TEST_CASE("a capture names the upvalues a function closed over", "[debugger]") {
	Store store("debug_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Line 6 is `return counter`, inside `bump` — where `step` is a local and
	// `counter` is an upvalue.
	runtime->Debug().Add("closure.luau", 6);
	REQUIRE(runtime->Run(CLOSURE, "closure.luau"));

	const std::span<const engine::script::DebugHit> hits = runtime->Debug().Hits();
	REQUIRE_FALSE(hits.empty());
	REQUIRE_FALSE(hits.front().Frames.empty());

	const engine::script::DebugFrame &frame = hits.front().Frames.front();

	const auto find = [](const std::vector<engine::script::DebugLocal> &named,
						 std::string_view wanted) -> const engine::script::DebugLocal * {
		for (const engine::script::DebugLocal &value : named) {
			if (value.Name == wanted) {
				return &value;
			}
		}
		return nullptr;
	};

	// **The argument is a local and the captured variable is an upvalue**, and
	// the two lists must not agree about either. A capture that merged them
	// would answer "what is in scope" and lose "where did it come from", which
	// is the question an upvalue is looked at to answer.
	const engine::script::DebugLocal *step = find(frame.Locals, "step");
	REQUIRE(step != nullptr);
	CHECK(step->Value == "5");
	CHECK(find(frame.Upvalues, "step") == nullptr);

	const engine::script::DebugLocal *counter = find(frame.Upvalues, "counter");
	REQUIRE(counter != nullptr);
	CHECK(counter->Value == "5");
	CHECK(find(frame.Locals, "counter") == nullptr);

	// A variable the function never mentions is neither. `bump` does not read
	// `label`, so Luau does not capture it — which is worth pinning, because a
	// capture that listed every enclosing local would be listing the wrong
	// thing and would look right.
	CHECK(find(frame.Upvalues, "label") == nullptr);
}

TEST_CASE("a chunk's own frame has locals and no upvalues", "[debugger]") {
	Store store("debug_test");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// Line 8 is `return counter`, at the chunk's top level — one frame out from
	// the case above.
	runtime->Debug().Add("closure.luau", 8);
	REQUIRE(runtime->Run(CLOSURE, "closure.luau"));

	const std::span<const engine::script::DebugHit> hits = runtime->Debug().Hits();
	REQUIRE_FALSE(hits.empty());

	const engine::script::DebugFrame &frame = hits.front().Frames.front();

	// At the top level `counter` is a local, not an upvalue — the opposite of
	// what the inner frame reported for the same name, which is what makes the
	// distinction observable rather than a claim.
	bool localCounter = false;
	for (const engine::script::DebugLocal &value : frame.Locals) {
		localCounter = localCounter || value.Name == "counter";
	}
	CHECK(localCounter);

	// **And it has no upvalues at all, which is Luau rather than a gap.** Lua
	// 5.2 gives every chunk an `_ENV` upvalue; Luau keeps a closure's
	// environment beside the upvalue array, so a main chunk closes over nothing.
	//
	// Worth a case of its own precisely because it is surprising: somebody
	// porting this capture from Lua would expect one entry here, and a version
	// that invented one to match would be reporting a variable the VM does not
	// have.
	CHECK(frame.Upvalues.empty());
}

// --- the service ----------------------------------------------------------------

namespace {
	// A studio's runtime, which is the only kind that gets the service.
	std::unique_ptr<engine::script::Runtime> StudioRuntime(Store &store) {
		engine::script::RuntimeLimits limits;
		limits.Role.Server = false;
		limits.Role.Client = false;
		limits.Role.Studio = true;
		return MakeRuntime(store, Language::Luau, limits);
	}
}

TEST_CASE("BreakpointService is a studio's and nobody else's", "[debugger]") {
	Store store("debug_test");
	engine::scene::EnsureClassTree();

	// **Absent rather than refusing.** A service that existed and answered "not
	// in a game" to everything is a surface somebody writes against and then
	// finds does nothing where it matters — and arming a breakpoint costs the
	// whole runtime its speed, which a shipped server must not let a game script
	// decide.
	const auto game = MakeRuntime(store, Language::Luau);
	REQUIRE(game->Run("assert(BreakpointService == nil, 'a game script got the debugger')"));
	CHECK_FALSE(game->Run("game:GetService('BreakpointService')"));

	const auto studio = StudioRuntime(store);
	REQUIRE(studio->Run(
		"assert(type(BreakpointService) == 'table', 'no service')\n"
		"assert(game:GetService('BreakpointService') == BreakpointService, 'two objects')\n"
	));
}

TEST_CASE("the service arms and disarms the same debugger the runtime reads", "[debugger]") {
	Store store("debug_test");
	engine::scene::EnsureClassTree();

	const auto runtime = StudioRuntime(store);

	// **One object, reached two ways.** The editor's panel writes
	// `Runtime::Debug()` and a tool writes the service; a second list would be
	// two things to keep in step and a breakpoint that fired in one place and
	// not the other.
	REQUIRE(runtime->Run(
		"assert(BreakpointService:IsArmed() == false, 'armed before anything was set')\n"
		"BreakpointService:SetBreakpoint('probe.luau', 4)\n"
		"assert(BreakpointService:IsArmed(), 'not armed after setting one')\n"
	));

	REQUIRE(runtime->Debug().Breakpoints().size() == 1);
	CHECK(runtime->Debug().Breakpoints().front().Source == "probe.luau");
	CHECK(runtime->Debug().Breakpoints().front().Line == 4);

	// And it fires, which is the half a list alone would not prove.
	REQUIRE(runtime->Run(PROGRAM, "probe.luau"));
	REQUIRE_FALSE(runtime->Debug().Hits().empty());
	CHECK(runtime->Debug().Hits().front().Line == 4);

	REQUIRE(runtime->Run(
		"local held = BreakpointService:GetBreakpoints()\n"
		"assert(#held == 1, 'the list did not come back')\n"
		"assert(held[1].Source == 'probe.luau' and held[1].Line == 4, 'the wrong entry')\n"
		"assert(held[1].Enabled and not held[1].Stops, 'the wrong defaults')\n"
		"assert(held[1].Hits > 0, 'the hit count did not cross')\n"
		"\n"
		"local hits = BreakpointService:GetHits()\n"
		"assert(#hits >= 1, 'no hits came back')\n"
		"assert(hits[1].Line == 4, 'the wrong line')\n"
		"assert(#hits[1].Frames >= 1, 'no frames')\n"
		"assert(type(hits[1].Frames[1].Locals) == 'table', 'no locals')\n"
		"assert(type(hits[1].Frames[1].Upvalues) == 'table', 'no upvalues')\n"
		"\n"
		"BreakpointService:ClearHits()\n"
		"assert(#BreakpointService:GetHits() == 0, 'the hits survived a clear')\n"
		"assert(#BreakpointService:GetBreakpoints() == 1, 'clearing hits took a breakpoint')\n"
		"\n"
		"assert(BreakpointService:SetEnabled('probe.luau', 4, false), 'nothing to disable')\n"
		"assert(BreakpointService:IsArmed() == false, 'still armed with the only one off')\n"
		"\n"
		"assert(BreakpointService:RemoveBreakpoint('probe.luau', 4), 'nothing to remove')\n"
		"assert(#BreakpointService:GetBreakpoints() == 0, 'the list survived a remove')\n"
	));
}

TEST_CASE("the service takes a script instance as well as a path", "[debugger]") {
	Store store("debug_test");
	engine::scene::EnsureClassTree();
	engine::script::RegisterScriptComponents();

	store.SetResource(engine::script::SourceCache{});
	store.ResourceMutable<engine::script::SourceCache>()->Set(Name("probe.luau"), PROGRAM);

	const engine::ecs::Entity script = engine::script::MakeScript(store, "probe.luau", "Probe", false);
	REQUIRE(script != engine::ecs::NULL_ENTITY);

	const auto runtime = StudioRuntime(store);

	// **The high level is the one a tool has in its hand.** It has just walked
	// the tree and found a `LuaSourceContainer`; resolving the instance's
	// `Source` itself would be reimplementing the one mapping that has to agree
	// with what the VM reports.
	REQUIRE(runtime->Run(
		"local probe = workspace.Parent and nil or nil\n"
		"for _, found in World:Query('script.Source') do probe = found end\n"
		"assert(probe ~= nil, 'the script instance was not found')\n"
		"BreakpointService:SetBreakpoint(probe, 4)\n"
	));

	REQUIRE(runtime->Debug().Breakpoints().size() == 1);
	CHECK(runtime->Debug().Breakpoints().front().Source == "probe.luau");

	// The near-misses a tool hits: a line that starts at zero, and an instance
	// that carries no source.
	CHECK_FALSE(runtime->Run("BreakpointService:SetBreakpoint('probe.luau', 0)"));
	CHECK_FALSE(runtime->Run("BreakpointService:SetBreakpoint(Instance.new('Part'), 1)"));
	CHECK_FALSE(runtime->Run("BreakpointService:SetBreakpoint(7, 1)"));
}
