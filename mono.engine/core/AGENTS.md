# core — module invariants

The root `AGENTS.md` carries policy. This file carries the invariants that are
specific to `core`, and breaking one of them is a review blocker rather than a
style note.

`core` is L0 (platform) and L1 (values). Everything else in the engine is above
it, so a mistake here is a mistake everywhere.

## What may go in here

- **L0** — OS abstraction, filesystem, logging, assertions, the clock, the
  metrics sink, and Tracy instrumentation.
- **L1** — pure value types with no runtime behind them: `Vector3`, `Color3`,
  `CFrame`, and the math on them.

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

`Profiling.hpp` is the *engine* profiler — Tracy zones, plus the `FrameGraph`
scope tree the F5 overlay draws. The userland profiler is a Luau CPU sampler and
lives in `script/src/profiling/`. They share no code and should not grow any.

## The frame graph is about the frames you are not looking at

Three of its features exist for a spike that a per-frame panel structurally
cannot show, because the panel repaints faster than a person can read:

- `RecentMaximum` — the worst single reading over the last 300 frames. A
  *reading*, not a total: a span that opens six times a frame contributes its
  worst of the six, so the number compares with the per-frame figure beside it.
- the retained window — five seconds, bounded by frames as well as by time,
  because at a few thousand frames a second the time bound alone decides the
  memory.
- `WriteSnapshot` — percentiles per span, then the worst frames and what was in
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
caller must own the string for the life of the run — the scheduler does.

The overlay reads the published spans *after* the frame that produced them has
ended, so a view into a local is a dangling read by the time it is drawn. The
copying form is the default for exactly that reason; STABLE exists only so the
scheduler does not pay for a copy it does not need.

## Depth is tracked past the budget, and that is not an oversight

Nothing below `MAXIMUM_DEPTH` is recorded, but the depth still moves — and the
matching close still moves it back. Skip that and every *sibling* after a
dropped subtree is recorded several levels too deep, which corrupts the whole
tree below it rather than just losing the part that overflowed.

## FrameGraph records one thread

`FrameGraph` collects from whichever thread called `BeginFrame`. Scopes opened
on a worker are dropped and counted in `Dropped()`, deliberately: a lock-free
multi-thread span collector is Tracy's job, and Tracy is already here. Do not
add locking to make it multi-thread — attach a profiler instead.

## `Name` ids are session-local, and that is load-bearing

`Name` interns a string and hands back a counter. The counter is assigned in
first-seen order, so it differs between two runs that load things in a different
order — which is fine, because it never leaves the process.

**Do not serialize `Name::Id()`.** Not into a save file, not onto the wire, not
into a manifest. The string is the contract; the id is an optimisation that
happens to be visible. A wire format that needs to be compact sends a string
table once and indices after, which is a different number with a different
lifetime.

`Reserve` exists for the case where a number is itself part of a format and was
written down by hand. It refuses a contradiction — two names on one id, or one
name on two — because both make two different things compare equal. Prefer not
to need it.

The registry never removes an entry and the storage never moves, which is what
lets `Text()` hand out a `string_view` that stays valid. That is why it is a
deque rather than a vector; changing it to a vector would dangle every view
already given out, and the corruption would appear far from the change.

## Value types stay values

No `Vector3` method may allocate, log, or reach a global. They are used by tools
with no engine around them.
