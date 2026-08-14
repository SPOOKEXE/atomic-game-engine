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

## The metrics sink is a seam, not a convenience

`core::Metrics` exists so that `net` at L11 can report bytes-per-remote to the
userland profiler at L13 without `net` depending on `script`. It is deliberately
a write-only sink with no reader in this module.

Do not add a `Metrics::Get(name)`. The moment a subsystem reads another
subsystem's counter, the sink has become a global variable with extra steps.

## There are two profilers and this is one of them

`Profiling.hpp` is the *engine* profiler - Tracy zones, plus the `FrameGraph`
scope tree the F5 overlay draws. The userland profiler is a Luau CPU sampler and
lives in `script/src/profiling/`. They share no code and should not grow any.

## The frame graph is about the frames you are not looking at

Three of its features exist for a spike that a per-frame panel structurally
cannot show, because the panel repaints faster than a person can read:

- `RecentMaximum` - the worst single reading over the last 300 frames. A
  *reading*, not a total: a span that opens six times a frame contributes its
  worst of the six, so the number compares with the per-frame figure beside it.
- the retained window - five seconds, bounded by frames as well as by time,
  because at a few thousand frames a second the time bound alone decides the
  memory.
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
