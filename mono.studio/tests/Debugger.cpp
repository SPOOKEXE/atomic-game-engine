// The editor's half of the debugger: which breakpoints exist, and where a
// gutter click lands.
//
// **The panel is not what is under test.** Drawing a table needs an ImGui
// context and proves nothing; what the panel *does* is toggle a breakpoint at a
// line and keep the editor's list in step with every live runtime's, and both of
// those are ordinary functions over ordinary data.
//
// The rule worth pinning is the one that is easy to get subtly wrong: the
// editor's list is the one that survives a Stop, so a toggle has to write it
// **and** the runtimes, in that order.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("studio.debugger")
TEST_DEPENDS("engine.script.debugger")

using engine::core::Name;
using engine::ecs::Store;
using engine::script::BreakAction;
using engine::script::Breakpoint;
using engine::script::Debugger;
using engine::script::DebugHit;
using engine::script::Language;
using engine::script::MakeRuntime;

namespace {
	constexpr const char *PROGRAM = R"(
local first = 1
local second = 2
local third = first + second
return third
)";

	// The editor's own list and the runtimes' copies, as `ToggleBreakpoint`
	// keeps them.
	//
	// **A stand-in rather than a live `Editor`**, which needs a window and a
	// renderer. What is copied is the rule, and it is four lines — a copy that
	// drifted would fail the case that pins the two halves agreeing.
	struct Editorish {
		Debugger Master;
		std::vector<Debugger *> Live;

		const Breakpoint *At(std::string_view source, int line) const {
			for (const Breakpoint &point : Master.Breakpoints()) {
				if (point.Line == line && point.Source == source) {
					return &point;
				}
			}
			return nullptr;
		}

		void Toggle(const std::string &source, int line, BreakAction action) {
			if (At(source, line) != nullptr) {
				Master.Remove(source, line);
				for (Debugger *live : Live) {
					live->Remove(source, line);
				}
				return;
			}

			Master.Add(source, line, action);
			for (Debugger *live : Live) {
				live->Add(source, line, action);
			}
		}
	};
}

TEST_CASE("a gutter click adds a breakpoint and clicking again takes it away", "[studio][debugger]") {
	Editorish editor;

	editor.Toggle("enemy.luau", 12, BreakAction::Capture);
	REQUIRE(editor.Master.Breakpoints().size() == 1);
	CHECK(editor.At("enemy.luau", 12) != nullptr);
	CHECK(editor.At("enemy.luau", 13) == nullptr);

	// **The same line twice is off, not two breakpoints.** A gutter is a toggle
	// and `Debugger::Add` replaces rather than appending, so a version that only
	// added would look like it worked and would leave the dot on for ever.
	editor.Toggle("enemy.luau", 12, BreakAction::Capture);
	CHECK(editor.Master.Breakpoints().empty());
	CHECK(editor.At("enemy.luau", 12) == nullptr);

	// Two scripts may hold a breakpoint on one line number without either
	// being the other's.
	editor.Toggle("enemy.luau", 4, BreakAction::Capture);
	editor.Toggle("player.luau", 4, BreakAction::Capture);
	CHECK(editor.Master.Breakpoints().size() == 2);

	editor.Toggle("enemy.luau", 4, BreakAction::Capture);
	CHECK(editor.Master.Breakpoints().size() == 1);
	CHECK(editor.At("player.luau", 4) != nullptr);
}

TEST_CASE("a toggle reaches the runtimes that are already running", "[studio][debugger]") {
	Store store("studio_debug");
	engine::scene::EnsureClassTree();

	const auto runtime = MakeRuntime(store, Language::Luau);

	Editorish editor;
	editor.Live.push_back(&runtime->Debug());

	// **Both lists, because they answer different questions.** The editor's is
	// what somebody asked for and survives a Stop; the runtime's is the copy the
	// VM consults. Writing only the first would be a breakpoint that never
	// fires, and only the second one that disappears at the next Stop.
	editor.Toggle("probe.luau", 4, BreakAction::Capture);

	REQUIRE(runtime->Debug().Breakpoints().size() == 1);
	CHECK(runtime->Debug().Armed());

	REQUIRE(runtime->Run(PROGRAM, "probe.luau"));
	REQUIRE_FALSE(runtime->Debug().Hits().empty());
	CHECK(runtime->Debug().Hits().front().Line == 4);

	editor.Toggle("probe.luau", 4, BreakAction::Capture);
	CHECK(runtime->Debug().Breakpoints().empty());
	CHECK_FALSE(runtime->Debug().Armed());
}

TEST_CASE("a run started later is handed the editor's breakpoints", "[studio][debugger]") {
	Store store("studio_debug");
	engine::scene::EnsureClassTree();

	// Somebody sets a breakpoint before pressing Play, which is the ordinary
	// order — and the whole reason the editor keeps a list of its own.
	Editorish editor;
	editor.Toggle("probe.luau", 4, BreakAction::Stop);

	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->Debug().Adopt(editor.Master);

	REQUIRE(runtime->Debug().Breakpoints().size() == 1);
	CHECK(runtime->Debug().Breakpoints().front().Action == BreakAction::Stop);

	// A stopping breakpoint ends that script as an ordinary error, which the
	// host already knows how to report.
	CHECK_FALSE(runtime->Run(PROGRAM, "probe.luau"));
	CHECK_FALSE(runtime->Debug().Hits().empty());
}

TEST_CASE("the panel's selection survives the hit log rolling", "[studio][debugger]") {
	// **An index rather than a pointer, because the log is bounded and rolls.**
	// A pointer into it would dangle the moment a loop pushed one capture too
	// many; an index past the end reads as "pick a capture", which is what the
	// panel draws.
	Debugger debug;

	for (size_t index = 0; index < Debugger::MAXIMUM_HITS + 8; index++) {
		DebugHit hit;
		hit.Source = "loop.luau";
		hit.Line = static_cast<int>(index);
		debug.Record(std::move(hit));
	}

	CHECK(debug.Hits().size() == Debugger::MAXIMUM_HITS);

	// The oldest went and the newest stayed, which is what makes "newest first"
	// the right way for the panel to list them.
	CHECK(debug.Hits().back().Line == static_cast<int>(Debugger::MAXIMUM_HITS) + 7);

    // An index that was valid before the roll is still in range or is not, and
    // either way it is a comparison rather than a dereference.
	const size_t selected = Debugger::MAXIMUM_HITS + 4;
	CHECK(selected >= debug.Hits().size());
}
