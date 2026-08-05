#pragma once

// Breakpoints, and what was on the stack when one was reached.
//
// ## What this is, and what it deliberately is not
//
// **It captures, it does not pause.** A breakpoint here records the call stack
// and every local in scope at the moment the line ran, and then execution
// carries on — or stops that one script, if that is what the breakpoint asked
// for. What it never does is hold the VM inside the line while somebody looks.
//
// Two reasons, and both are load-bearing rather than temporary:
//
//   - **A paused world is not a replayable one.** Rule 5 is that work inside a
//     tick may be parallel and work across ticks may not; a tick held open for
//     four minutes while somebody reads a variable is the largest possible
//     violation of it. A recording made through a debugging session would not
//     replay, and `just replay-check` would fail a long way from the cause.
//   - **The editor's frame loop is this thread.** Blocking inside the hook
//     blocks the loop that would draw the panel showing what was hit, so a
//     stop-the-world breakpoint in a single-threaded editor is a frozen window
//     with the answer trapped inside it.
//
// A stepping debugger wants the VM on a thread of its own with the interface on
// another, which is a different program and a decision v0.11 can take
// deliberately. Until then this answers "what was true when that line ran",
// which is the question most breakpoints are actually asked.
//
// ## What it costs when nothing is armed
//
// Nothing. Luau's single-step mode is switched on only while at least one
// breakpoint exists and off again when the last one goes, so a runtime nobody
// is debugging runs exactly as it did before this file existed. That is the
// property worth protecting in any change here — a debugger that costs
// something when unused is one that gets compiled out and then rots.
//
// @tier L9 · shared

#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// What a breakpoint does when its line runs.
	//
	// @since v0.10
	enum class BreakAction : uint8_t {
		// Record the stack and carry on. The script finishes; the panel shows
		// what was true as it went past.
		Capture,

		// Record the stack and raise, which ends this script's run.
		//
		// **An ordinary script error, not a special state.** The host already
		// knows what to do with one — it is logged, the remaining scripts still
		// run, and `LastError` names it — so stopping needs no new path through
		// the runtime and cannot leave the VM somewhere nothing expects.
		Stop,
	};

	// A place execution should be reported from.
	//
	// @since v0.10
	struct Breakpoint {
		// The chunk name, which is the script's asset-relative path.
		//
		// Matched as a suffix rather than exactly, because Luau reports a chunk
		// by the name it was loaded under and a caller thinks in file names.
		std::string Source;

		// The 1-based line.
		int Line = 0;

		// What happens when it is reached.
		BreakAction Action = BreakAction::Capture;

		// Whether it is armed. Kept rather than removed when switched off, so
		// somebody narrowing down a problem does not retype the line number.
		bool Enabled = true;

		// How many times it has been reached since it was added.
		uint64_t Hits = 0;
	};

	// One named value in scope at a captured frame.
	//
	// @since v0.10
	struct DebugLocal {
		// What the script calls it.
		std::string Name;

		// Its value, rendered the way `print` would render it — so an instance
		// reads as its name and a Vector3 as its components rather than as an
		// address.
		std::string Value;
	};

	// One activation record, innermost first.
	//
	// @since v0.10
	struct DebugFrame {
		// The chunk this frame is executing.
		std::string Source;

		// The function's name, or empty for a chunk's top level.
		std::string Function;

		// The line it is on.
		int Line = 0;

		// Every local and argument in scope.
		std::vector<DebugLocal> Locals;
	};

	// What was true when a breakpoint was reached.
	//
	// @since v0.10
	struct DebugHit {
		// Where it stopped.
		std::string Source;

		// The line.
		int Line = 0;

		// The script instance that was running, when one is known.
		ecs::Entity Instance;

		// The stack, innermost frame first.
		std::vector<DebugFrame> Frames;
	};

	// The breakpoints a runtime honours, and what they caught.
	//
	// **One per runtime, reached through `Runtime::Debug`.** Not a global: two
	// runtimes over two worlds must not share breakpoints for the same reason
	// they must not share a store, and a file-static would have made that
	// mistake available.
	//
	// @since v0.10
	class Debugger {
	  public:
		// Adds a breakpoint, replacing any at the same place.
		//
		// Replacing rather than appending, because two breakpoints on one line
		// is two reports of one event and no way to tell which is which.
		//
		// @param source The chunk name, matched as a suffix.
		// @param line   The 1-based line.
		// @param action What to do when it is reached.
		void Add(std::string source, int line, BreakAction action = BreakAction::Capture);

		// Removes the breakpoint at a place.
		//
		// @param source The chunk name.
		// @param line   The line.
		// @return `true` when one was there.
		bool Remove(std::string_view source, int line);

		// Switches one on or off without forgetting it.
		//
		// @param source  The chunk name.
		// @param line    The line.
		// @param enabled Whether it should fire.
		// @return `true` when one was there.
		bool Enable(std::string_view source, int line, bool enabled);

		// Forgets every breakpoint.
		void Clear();

		// Takes another debugger's breakpoints, keeping none of its hits.
		//
		// **What a fresh runtime is given at the start of a run.** Breakpoints
		// belong to the person debugging, not to the VM that happens to be
		// alive — a Stop destroys the runtime, and re-typing every line number
		// afterwards is how a debugger stops being used. Hits are deliberately
		// not carried: they describe a run that is over, and showing them
		// against a new one would be a lie about when they happened.
		//
		// Existing breakpoints are kept and matching ones replaced, so this can
		// be applied to a runtime that already has some.
		//
		// @param other The list to take.
		void Adopt(const Debugger &other);

		// Every breakpoint, in the order they were added.
		//
		// @return The list, valid until the next `Add` or `Clear`.
		std::span<const Breakpoint> Breakpoints() const {
			return Points;
		}

		// Whether anything would fire.
		//
		// **What decides whether single-step mode is on.** A runtime with no
		// enabled breakpoint runs at full speed, which is what keeps this
		// feature free when it is not in use.
		//
		// @return `true` when at least one breakpoint is enabled.
		bool Armed() const;

		// The breakpoint covering a place, or null.
		//
		// @param source The chunk name reported by the VM.
		// @param line   The line being executed.
		// @return The breakpoint, or null when execution should carry on.
		Breakpoint *Match(std::string_view source, int line);

		// What has been caught, oldest first.
		//
		// @return The hits, valid until the next `Record` or `ClearHits`.
		std::span<const DebugHit> Hits() const {
			return Caught;
		}

		// Records a hit, dropping the oldest when the log is full.
		//
		// **Bounded, because a breakpoint inside a loop is the ordinary case.**
		// A line that runs ten thousand times a tick would otherwise fill memory
		// with stack captures faster than anybody could read one.
		//
		// @param hit What was true.
		void Record(DebugHit hit);

		// Forgets every hit, leaving the breakpoints alone.
		void ClearHits() {
			Caught.clear();
		}

		// The most hits kept at once.
		static constexpr size_t MAXIMUM_HITS = 64;

	  private:
		std::vector<Breakpoint> Points;
		std::vector<DebugHit> Caught;
	};
}
