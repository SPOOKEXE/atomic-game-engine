# core - module invariants

The root `AGENTS.md` carries policy. This file carries the invariants that are
specific to `core`, and breaking one of them is a review blocker rather than a
style note.

`core` is L0 (platform) and L1 (values). Everything else in the engine is above
it, so a mistake here is a mistake everywhere.

## What may go in here

- **L0** - OS abstraction, filesystem, logging, assertions, the clock, the
  metrics sink, and Tracy instrumentation.
- **L1** - pure value types with no runtime behind them: `Vector3`, `Color3`,
  `CFrame`, and the math on them.
- **L1** - a format several modules have to agree on, when reading or writing it
  needs nothing but the standard library. `Bytes` is the byte layout everything
  serialises through, `Config` reads a settings file, and `Xml` is the tag
  scanner three formats above here run on. **The test is the dependency, not the
  usefulness**: something that needs another module's types belongs in that
  module however many callers it would have here.

## What may not

- **No windowing, no graphics, no audio, no VM.** The delivery service links
  `core` on a machine with no SDL and no Vulkan SDK, and CI asserts that. A
  `#include <SDL3/...>` anywhere under this module breaks it.
- **No operating-system name in a public header.** Platform code lives in
  `src/platform/<os>/` behind a header that names no OS. That discipline is what
  makes a private platform overlay a mechanical extraction later rather than a
  refactor.
- **No dependency on any other engine module.** `core` is the bottom. If
  something here needs `ecs`, it belongs in `ecs`.

## The log is guarded, categorised, and has a size budget

`Log.hpp` is included by about two hundred and fifty translation units, so what
goes in it is a decision about the whole engine's compile time rather than about
this file. **The budget is the preprocessed line count and it is measured, not
asserted**: 22,842 before v0.19 and 23,016 after, with the flags
`compile_commands.json` gives for `Log.cpp`, and all 174 of those lines are
declarations in that file rather than a header that arrived with them.
`Clock.hpp` is 422 and is the bar.

Three things follow, and each of them is why something in that header looks
odd:

- **No `<atomic>`** - 7,452 lines on its own. A category's level is read with
  the compiler's atomic builtin on a plain `unsigned char`, and `Log.cpp` writes
  it with the matching store. Both halves are spelled the same way on purpose.
- **No `Name.hpp`** - 51,820 lines. A category *is* a `core::Name`, and
  `LogCategory` holds the interned id as a bare integer and asks `Log.cpp` for
  the text.
- **No `fmt::format`** - `<spdlog/fmt/bundled/base.h>` is what declares the
  argument erasure, and that is all this header needs. `Assert.hpp` inherits the
  same constraint, which is why `ENGINE_ASSERT_MSG` hands its arguments to
  `Assert::FailWith` rather than formatting them at the call site.

**A disabled statement evaluates nothing, and that took a sweep to switch on.**
The macros used to run their arguments whether the level was on or not, because
guarding them would silently stop running any argument with a side effect. All
711 call sites were swept - `++`, compound assignment, `fetch_add`, `Pop`,
`Drain`, `release`, `Metrics::`, and every distinct function named inside an
argument list - and **none of them had one**, so the guard went in and no call
site had to change. If you add a log statement whose argument mutates
something, you have written a bug that only appears at one log level.

**A log statement is a statement and not an expression.** It expands to a
`do { } while (false)`, so `REQUIRE_NOTHROW(ENGINE_WARN(...))` and
`condition ? ENGINE_ERROR(...) : ...` do not compile. Neither shape existed in
the tree; both would have been a log statement whose evaluation depended on
something other than its level.

**A category comes from the build, not from the call site.**
`mono_add_library` defines `ENGINE_LOG_CATEGORY` as the module's own name on
every target, which is how 711 call sites gained a category without one of them
being edited. That is the whole of "one category per module by convention" -
applied by the build rather than remembered by people. A file logging on behalf
of another area passes one explicitly to `ENGINE_LOG`, which is the general form
the five named macros expand to. There is deliberately no second family of
uncategorised macros: two ways to do one job is the debt that outlives whoever
added it.

**The compiled floor is not a level, it is a preprocessor decision.**
`ENGINE_LOG_COMPILED_LEVEL` is public on `Engine::core`, `trace` everywhere
except `release` and `bench`, where it is `debug`. Below it a macro expands to a
`sizeof` of an unevaluated call - so the format string is still checked, the
arguments are still type-checked, a variable used only by a compiled-out
statement is still used, and no instruction is emitted. Checked by looking:
`store '{}' created` is in the `dev` object for `ecs/src/Store.cpp` and is not
in the `release` one. `ENGINE_LOG` cannot be filtered this way, because its
level is an expression and the preprocessor cannot see one.

Measured at `preset=bench`: a disabled statement is under a nanosecond, with or
without an argument; an enabled one into a null sink is 84 ns; a throttled one
while it is quiet is 20 ns, which is one clock read.

## `ENGINE_ASSERT` exists now, and `release` is the interesting column

This file claimed an assertion facility from its first version and there was
none. What the engine had instead was **one** `assert()`, at
`render/src/ResourcePreview.hpp:34`, and every other module in the tree with no
invariant check at all - which is the telling part, because a facility nobody
has is a facility nobody reaches for. `Assert.hpp` is that facility, and the three macros
differ in exactly one way: what a `release` build does.

| Macro | `dev`, `ci`, `server`, `cdn` | `release`, `bench` |
|---|---|---|
| `ENGINE_ASSERT`, `ENGINE_ASSERT_MSG` | checks, reports, aborts | not compiled at all |
| `ENGINE_UNREACHABLE` | reports, aborts | reports, aborts |
| `ENGINE_ENSURE` | checks, reports, yields `false` | the same |

`MONO_ASSERTS` is arranged exactly as `MONO_HEAP_PROFILE` is and for the same
argument: a developer build checks its invariants without being reconfigured
first, and a shipped build does not pay for a check on a hot path.
`ENGINE_UNREACHABLE` is not switchable because there is nothing to carry on
into, and `ENGINE_ENSURE` is not switchable because it is the one for a fact
that might legitimately be false.

**It goes through the log sink, and that is the whole reason it is here rather
than being `assert()` with a nicer message.** The line is composed into one
buffer and handed over in a single `log()` call, so an invariant that fails on a
job worker is a line rather than fragments interleaved with three other workers.
The thread id is in the pattern for the same reason. It is written unfiltered:
an invariant that failed is not something a log setting may hide.

`Assert::SetHandler` replaces what runs *after* the line. The default flushes
and aborts. A handler that returns is what lets `tests/Assert.cpp` check that an
assert fired without ending the suite; nothing shipped should install one.

## The metrics sink is a seam, and its rule is about control flow

`core::Metrics` exists so that `net` at L11 can report bytes-per-remote to the
userland profiler at L13 without `net` depending on `script`. Writers name a
counter; the sink does not know or care who reads.

Until v0.19 this file said **"do not add a `Metrics::Get(name)`"**, and the
argument was right: the moment a subsystem reads another subsystem's counter to
decide something, the sink has become a global variable with extra steps. What
that also prevented was *reporting*, which is a different thing and was the
actual gap - the headless server had counted things since v0.9 and read none of
them, and `FrameGraph`'s dropped-span count was reachable only from the F5
overlay, which a server does not have.

So the rule is now the one it always meant:

> **Read to report. Never read to decide.**

A caller that branches on `Get` is doing the thing this section refuses. A
caller that prints, draws, folds or exports is what the read side is for.
`Snapshot` is shaped for exactly that: one lock, resets nothing, sorted by name.

`Drain` is still the counter reader and still has **exactly one caller per
frame**, because that is what makes a counter a per-frame rate rather than a
total that only goes up. Gauges and histograms are not drained: a gauge is a
level and a histogram's window is what its percentiles are over.

## Four counter mechanisms, and only one of them was a duplicate

`core::Metrics`, `core::FrameGraph`, `core::HeapProfile` and `ecs::Scheduler`'s
per-system timings all count things, and three of the four have a reason that
survives review: `FrameGraph` needs tree structure and per-frame identity,
`HeapProfile` is reached from inside `operator new` and would recurse if it took
`Metrics`' lock, and the scheduler's timings are drained in system order by a
panel that draws them in that order.

**What was missing was not one write side. It was one read side.** So
`FrameGraph::EndFrame` counts its dropped spans into
`FrameGraph::DROPPED_COUNTER`, and the server drains and reports the sink - and
a drop that no overlay is open to see is now a line in a headless run's report.
Nothing is counted on a frame that dropped nothing, so an absent row means "lost
no spans" rather than "nobody looked".

The one genuine duplicate left is `ecs::ChunkPool`'s `Allocated`/`Reused`
atomics, which are process-global, read only by tests, and would be two
`Metrics::Count` calls. That is an `ecs` change, not a `core` one.

## There are three profilers and two of them are here

`Profiling.hpp` is the *engine* profiler, and one macro feeds three consumers -
Tracy zones, the `FrameGraph` scope tree the F5 overlay draws, and a
`HeapProfile` tag. The userland profiler is a Luau CPU sampler and lives in
`script/src/profiling/`. It shares no code with these and should not grow any.

**The heap tag is on `ENGINE_PROFILE` rather than on a macro of its own, and
that is the whole reason the heap profiler is granular.** Several hundred scopes
are already placed where the work is; a second family of macros beside them
would be a second set of placements to keep in step, which is right on the day
it is written and wrong a month later. `ENGINE_HEAP_SCOPE` is for what allocates
and is not worth timing - a worker thread, an audio callback.

## A heap tag names a subsystem, never an instance of one

The tag tree is bounded and **never removes a node**. So a caller naming a tag
per script chunk, per entity or per asset fills it, and everything after that is
charged to an ancestor and counted in `HeapTotals::DroppedScopes`.

That is why `ENGINE_PROFILE_DYNAMIC` tags with its *fallback literal* and not
with the runtime name it hands `FrameGraph`. `ENGINE_PROFILE_DYNAMIC_STABLE`
does pass its name through, because its callers name a bounded set - the
scheduler names its systems - and the tree copies what it is given rather than
keeping the view. Copying is not an optimisation to remove: a borrowed view
would need storage living as long as the *process*, and a scheduler's names live
only as long as their world.

## The allocator hooks are a compile-time decision and cannot be a runtime one

A block allocated with no header and freed through the tracking
`operator delete` would have four words of somebody else's memory read as its
header. `MONO_HEAP_PROFILE` therefore decides once, for a whole program, and
there is deliberately no switch that turns headers off in a running process.
What *is* runtime is sampling, which only costs a walk of the tag tree.

**Nothing in `HeapProfile.cpp` may have a constructor.** It is reached from
inside `operator new`, so the first allocation in the process happens before any
dynamic initialiser could have run. Every global there is `constinit`, and that
is load-bearing rather than decorative: written without it, the tag tree's
`std::atomic` members were *dynamically* initialised and zeroed out every
allocation an earlier translation unit's static constructor had already made.
The symptom was a process holding 1689 live blocks and a tag tree that had only
ever seen 123 of them.

## The frame graph is about the frames you are not looking at

Three of its features exist for a spike that a per-frame panel structurally
cannot show, because the panel repaints faster than a person can read:

- `RecentMaximum` - the worst single reading over the last 300 frames. A
  *reading*, not a total: a span that opens six times a frame contributes its
  worst of the six, so the number compares with the per-frame figure beside it.
- the retained window - five seconds, bounded by frames *and* by readings.
  Seconds alone is not a bound on memory: how many frames five seconds holds is
  the frame rate. `MAXIMUM_HISTORY_READINGS` is the figure that actually decides
  what the window costs, and it is the one that binds first above a thousand
  frames a second - so the window is shallower there and `HistorySeconds` says
  so rather than assuming.
- `WriteSnapshot` - percentiles per span, then the worst frames and what was in
  each.

Two invariants hold them up, and both are quiet when broken:

- **Every tracked span gets a slot every frame, including the ones that did not
  run.** A span absent from a frame scored zero in it. Without writing that
  zero, one expensive frame keeps its reading on the panel forever.
- **The window is cleared whenever collection is switched on or off.**
  Collection only runs while the panel is open, so keeping the last session's
  frames would put a gap of arbitrary length in the middle of a five-second
  window and give the column a worst case from a different scene.

## A name is either a literal or copied, and the two are different macros

`ENGINE_PROFILE_DYNAMIC` copies the text into a pool the frame owns, so the
caller may pass a local. `ENGINE_PROFILE_DYNAMIC_STABLE` does not, and its
caller must own the string for the life of the run - the scheduler does.

The overlay reads the published spans *after* the frame that produced them has
ended, so a view into a local is a dangling read by the time it is drawn. The
copying form is the default for exactly that reason; STABLE exists only so the
scheduler does not pay for a copy it does not need.

## Depth is tracked past the budget, and that is not an oversight

Nothing below `MAXIMUM_DEPTH` is recorded, but the depth still moves - and the
matching close still moves it back. Skip that and every *sibling* after a
dropped subtree is recorded several levels too deep, which corrupts the whole
tree below it rather than just losing the part that overflowed.

## FrameGraph records one thread

`FrameGraph` collects from whichever thread called `BeginFrame`. Scopes opened
on a worker are dropped and counted in `Dropped()`, deliberately: a lock-free
multi-thread span collector is Tracy's job, and Tracy is already here. Do not
add locking to make it multi-thread - attach a profiler instead.

**The drop is reported rather than silent.** `EndFrame` adds each frame's drops
to `FrameGraph::DROPPED_COUNTER` in `core::Metrics`, because `Dropped()` was
read by the F5 overlay and by nothing else - and a headless server is precisely
the program that runs parallel compute and has no overlay.

## `Name` ids are session-local, and that is load-bearing

`Name` interns a string and hands back a counter. The counter is assigned in
first-seen order, so it differs between two runs that load things in a different
order - which is fine, because it never leaves the process.

**Do not serialize `Name::Id()`.** Not into a save file, not onto the wire, not
into a manifest. The string is the contract; the id is an optimisation that
happens to be visible. A wire format that needs to be compact sends a string
table once and indices after, which is a different number with a different
lifetime.

`Reserve` exists for the case where a number is itself part of a format and was
written down by hand. It refuses a contradiction - two names on one id, or one
name on two - because both make two different things compare equal. Prefer not
to need it.

The registry never removes an entry and the storage never moves, which is what
lets `Text()` hand out a `string_view` that stays valid. That is why it is a
deque rather than a vector; changing it to a vector would dangle every view
already given out, and the corruption would appear far from the change.

## `core` links no cryptography library, and `Random` is why it used to

`engine::core::Random` is SplitMix64's finaliser - Steele, Lea and Flood's
`mix64variant13`, OOPSLA 2014 - over the packed `(salt, index)` pair. It was
SHA-256 from Crypto++ until v0.15, and it plus `SecureWipe`'s zeroing loop were
the only two calls that made `Engine::core` link a cryptography library at all.
Both are written out in this module now and the `VENDOR` line is gone.

**Do not put one back.** Not for a better mixer, not for a checksum somebody
wants at L0 for an unrelated reason. Everything in the engine links `core`, so a
vendor here is a vendor everywhere. The argument for removing this one was never
binary size and adding one back cannot be justified on size either: `net` and
`assets` link Crypto++ themselves and are what put it in the client, the server
and the origin. `docs/retired/DEFERRED.md` D00004 has the per-program numbers,
measured either side of the swap, and they are identical.

**`Random` must never produce anything that has to be unpredictable.** A stream
is a pure function of two arguments an attacker can guess, and that determinism
is the entire reason the type exists - it cannot be fixed without destroying what
it is for. Every caller that needs entropy already draws from the operating
system through `net::Handshake` or `assets::GrantKey`, and each of those headers
says so where somebody would otherwise be tempted. `script`'s `GenerateGUID` is
the one that looks like an exception and is not: it stamps the version and
variant bits to make the *shape* a UUID and states in its own comment that the
value is neither unpredictable nor unique across processes.

**The sequence is pinned, and moving it is a breaking change to every game.**
`Random.new(seed)` in both script VMs draws through here, so the algorithm is
part of what a saved world means. `tests/Random.cpp` pins actual values rather
than comparing two runs - `just determinism` and `just replay-check` compare one
run against another and stay green on a generator that changed and agrees with
itself.

## `Xml` is a scanner and there is exactly one of it

`Xml.hpp` is here because it needs nothing, not because everybody wants it. It
opens no file, links no vendor and names no other module's type, so L1 is its
height; `assets` was the other candidate and is one tier above what the parser
depends on. **The reason it is at the bottom is that three modules above it were
each keeping the same refusals true** - `game` since v0.7, `Svg.cpp` since v0.13
and `bake/src/Xml.hpp` at v0.15 - and `bake` could not call `game`'s, because
`game` is L10. `D00128`, and the header carries the whole argument.

Three rules, and the third is the one somebody will get wrong:

- **It is not a document model and must not grow into one.** A caller drives
  `NextTag` and keeps its own stack. That is what makes depth a count somebody
  bounded rather than the C stack running out with no file named, and it is why
  `game`'s tree is in `game`.
- **A `<!DOCTYPE` or `<!ENTITY` is not parsed at all.** Not bounded, not
  configurable - there is no code here that could expand an entity, so there is
  no option that could switch one on. Everything this rejects, it rejects by
  construction rather than by default.
- **The two entity policies are both here and neither is the default.**
  `CheckEntityReferences` sweeps a document for a caller that never unescapes;
  `ReadContent` refuses at each point a reference is read and exempts CDATA, for
  a caller whose CDATA is a program. Collapsing them refuses a valid file - a
  real `.rbxmx` in the corpus carries the Luau pattern `"[&;]"` inside a
  script - and each caller has a case that goes red if somebody does.

## Value types stay values

No `Vector3` method may allocate, log, or reach a global. They are used by tools
with no engine around them.

## The flag layer is frozen before the loop starts, and that is rule 5

`core::Flags` holds process-wide settings and `core::Config` is the only thing
that reads a file, an environment or an `argv` into it. The split is the point:
a program adds a source without the store learning a fourth format.

**`Flags::Freeze` is not tidiness.** A value that can move between two ticks is
a value two machines can disagree about, and the disagreement arrives as a
desync a long way from the flag that caused it. Every program calls it once its
options are applied; a `Set` after it is refused and named in the log. Reads
after the freeze take no lock, which is what makes a flag safe in a hot path -
and reads *before* it are only safe on the thread doing the startup.

**Precedence is a property of the source and never of the call order.**
`Default < ConfigFile < Environment < CommandLine`, compared as an enum, so a
program may apply its sources in whichever order suits it and a file still
cannot overwrite what somebody typed. A `Set` that loses answers `Outranked`,
which is not a failure - it is the rule working, and it is reported so that "my
config file does nothing" is not a mystery.

**A flag is declared by a table a module hands over, never by a self-registering
static.** A static in a translation unit nothing else references is dropped by
the linker out of a static archive, and a flag that silently does not exist is
worse than one that is missing: the program runs, reads the built-in default and
reports nothing. `script::ServiceCatalogue` made this argument first.

**A key naming no declared flag is an error**, exactly as an undeclared option is
to `core::Arguments`, and for the same reason: a typo that is silently ignored
fails at the behaviour, days later, somewhere unrelated. That makes "declare
every table before applying any source" a real ordering constraint, and it is
one the build does not check - a program that declares late refuses its own
settings, loudly, at startup.

**A default that is derived belongs in a `FlagTableBuilder`.** A program's
built-in values live in its own `Options` struct and a content form's flag name
is built from the extension table; a `static constexpr` array of literals cannot
express either without writing the fact down twice, which is rule 2.
