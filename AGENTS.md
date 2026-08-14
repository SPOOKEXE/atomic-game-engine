# AGENTS.md - how AI contributes to this engine

This project uses AI models heavily and deliberately. That is a stated part of
the development cycle, not an embarrassment to be managed. What follows is how
to do it without the codebase turning into something nobody can maintain.

This file is policy. Each module has its own `AGENTS.md` carrying the
invariants specific to that module, and those are the ones that catch real
mistakes. **Read the module's `AGENTS.md` before changing anything in it.**

NEVER ADD AGENT AS CO-AUTHOR CREDITS OR GENERATED FOOTERS.

---

## RULES!

- Cannot edit AGENTS.md
- Cannot edit CLAUDE.md
- Cannot edit CONTRIBUTING.md
- Cannot edit README.md
- Cannot edit CODE_DOCUMENTING.md
- Cannot edit CODE_FORMAT.md
- Cannot edit CODE_QUALITY.md

Do NOT put random crap in these documents.
These are critical to AI operations.

## Exceptions

- Important changes (of which will be verified)

---

## MCP Servers

### codegraph (https://github.com/colbymchenry/codegraph#cli-reference)

Use `codegraph init` to index the codebase in a terminal.
Use `codegraph sync` to update your codebase index.
Use codegraph mcp tools to search codebase more effectively.

---

## Read these first, in this order

1. This file.
2. The `AGENTS.md` of every folder you are about to touch -
   `mono.engine/<module>/`, `mono.client/`, `mono.server/`, `mono.tools/`,
   `mono.vendor/`.
3. [`docs/CODE_FORMAT.md`](docs/CODE_FORMAT.md) - how the code looks.
4. [`docs/CODE_DOCUMENTING.md`](docs/CODE_DOCUMENTING.md) - where a comment ends
   up once `just docs` has run, and the tags that are available.
5. [`docs/CODE_QUALITY.md`](docs/CODE_QUALITY.md) - the checklist a change is
   reviewed against.
6. `ROADMAP.md` - what version we are on, and therefore what is in scope.

[`RUNNING.md`](RUNNING.md) is the reference for running anything - the client,
the server, one test suite, the tools. Read it before inventing a command line.

[`.claude/`](.claude/README.md) holds the skills and workflows checked in for
this repository - `/run-checklist` before a pull request, `/new-module` when
adding a module. They sit under these rules rather than beside them: a skill changes how
something is approached, not whether the layer stack applies.

Skipping step 2 is the most common way to produce a change that compiles,
passes, and is wrong.

---

## The six rules

### 1. The layer stack is not negotiable

Every module sits at a height, and **a layer may see every layer below it and
none above it.** The heights are in the design notes; the enforcement is in
`mono.build/MonoLibrary.cmake`, which fails at configure time with the
offending edge named.

If a change needs an edge that the tier check refuses, that is the design
telling you something. The fix is almost never `ALLOW_TIER_ESCAPE`.

### 2. The ECS owns the storage

A module does not keep private vectors or dirty flags for data another module
also reads. Two copies of the same fact drift apart the first time one of them
is updated inside a branch, and the resulting bug reproduces about once a week.

### 3. Nothing crossing a world boundary is a pointer

This is what keeps thread-per-world and process-per-world interchangeable.
Everything crossing is a message carrying a copy. One shared pointer added
because "it is only threads today" ends the process option permanently, and the
thing that ended it is invisible.

### 4. A name crosses boundaries. A number does not

Anything that has to survive a save file, a wire format, a manifest or a rename
of the file it was declared in is identified by its **string**. Ids derived from
declaration order are not stable - reorder two `add_subdirectory` lines and
every saved reference points somewhere else, silently.

Inside one process a string is the wrong thing to compare with, so `core::Name`
interns once and hands back a dense counter. Construction is a hash lookup;
everything after is an integer compare. Serializing `Name::Id()` undoes the
whole point of the type.

### 5. Work inside a tick may be parallel. Work across ticks may not

`Store::EachParallel` and `Jobs::For` both block until done, so a tick stays one
thing that starts and finishes and a recorded run still replays. A result that
lands a tick later on a slower machine is a desync.

Parallel is also not free: below a crossover it is *slower*, and the crossover
is higher than it looks. Measure in `release`, and put the number in a comment.

### 6. A rule the build does not check is documentation

If you introduce a constraint, either make the build enforce it or write down
in the relevant `AGENTS.md` that it is a convention. Do not leave a third
category of rules that exist only in somebody's memory.

---

## What a good change looks like here

**Finish the thing.** A feature that is half-added is worse than one not
started, because the next person cannot tell which half is intentional. If you
run out of room, leave the codebase in a state that builds and passes, and say
plainly what is missing.

**Write the test with the code, not after it.** Every `mono.X` folder carries
its own `tests/` - `mono.engine/<module>/tests/`, `mono.client/tests/`,
`mono.server/tests/`, `mono.tools/<tool>/tests/`. There is no central test
directory, because a test belongs with the thing it tests and moves when that
moves. `TEST_SUITE_ID` is hand-declared; the file set the runner uses is
derived, so you do not maintain a list.

**Every public header is covered by a suite, and the suite is usually named
after it.** One test file per header is the default, because a suite is one
file and that is the granularity the runner re-runs at - so a narrow suite
means a narrow re-run. That is measurable rather than theoretical: while
`Vector3`, `Color3` and `CFrame` shared one `Types.cpp`, touching `Color3.hpp`
re-ran all three plus everything downstream. Split, it re-runs two suites.

Three cases depart from the default deliberately:

- A type with no behaviour of its own is covered where it is used.
  `Entity.hpp` is a handle, and `Store.cpp` exercises it.
- A program's own headers are covered by the suite that drives them, because
  testing them apart from the loop that runs them tests nothing. `Client.hpp`
  and `Scene.hpp` are covered by `SceneTick.cpp`; `Simulation.hpp` by
  `Server.cpp`.
- **A header needing a GPU has no unit suite.** `Renderer.hpp` is the only one,
  and it is checked by running the client rather than by a test. Do not add a
  mock renderer to close the gap on paper.

Anything else without a suite is a gap, not a convention. Prefer to separate by subfolder and e2e for end-to-end,
but flat is OK.

**Comment the decision, not the mechanics.** `// increment the counter` above
`counter++` is noise.
`// Cycling hands back a fresh allocation rather than stalling on the copy the previous frame may still be reading` is the reason
somebody will need in six months. If a line looks wrong but is right, that is
exactly the line that needs a comment. Be aggressive in keeping comments short
where possible, no reason for five lines of code when it can be described in two.

**Delete the thing you replaced.** Two ways to do one job is the most expensive
kind of debt in a monorepo, because both accumulate callers.

**Prefer the boring option.** This engine is aiming at something large. The
budget for cleverness is best spent where the problem is genuinely hard, and
almost none of it is in the plumbing.

---

## What not to do

- **Do not add a module without adding it to
  `mono.tools/architecture/expected_graph.json`.** A new module is an
  architectural change and should show up in review as a diff to that file.
- **Do not widen a public header to make a test easier.** The test can link the
  module's `src/` if it must; that is what the private include directory is
  for.
- **Do not reach for a vendor library in a public header.** `VENDOR_PUBLIC` in
  `mono_add_library` exists for the cases where a type genuinely has to
  propagate, and every use of it widens what the rest of the engine can see.
- **Do not "fix" something you have not reproduced.** A plausible explanation
  that happens to be wrong costs more than no explanation, because it stops the
  search.
- **Do not silently reduce scope.** If part of a task turns out to be blocked,
  finish everything else and say explicitly what was left out and why.

---

## The tree

```
mono.build/    the tier system, MonoLibrary.cmake, the shared test main
mono.vendor/   submodules, shared by every mono.X that needs one
mono.engine/   the libraries, L0 to L13 - never a product
mono.client/   the client library and its thin main        [client]
mono.server/   the server library and its thin main        [server]
mono.tools/    architecture check, test runner, doc filter, and their tests
```

Every one of those carries `AGENTS.md`, `docs/` and - except `mono.build` and
`mono.vendor` - its own `tests/`.

`mono.engine` is libraries only: no `main.cpp` lives under it and nothing ships
from it alone. `mono.client` and `mono.server` are each a library plus a thin
executable, which is what makes single-player possible later - the client links
the server library and hosts one in-process.

## Performance

First-party code builds at `-O0` by default, and the `release` preset is the
one place that changes. This is deliberate: a profile should measure what the
engine does rather than what the optimiser rescued. An algorithm that is only
fast at `-O2` is a slow algorithm with a fast compiler.

Two profilers, and they are not the same thing:

- **Tracy** - the real one. `ENGINE_PROFILE(...)`, a second process, every
  thread, full history. On-demand, so it costs nothing until something attaches.
- **The F5 overlay** - the in-game frame graph, from the same macros. It has to
  work with nothing attached, on a machine that is not yours.

The userland profiler, when it arrives, is a third thing and shares no code
with either.

---

## Honesty in reporting

If tests fail, say so and show the output. If a step was skipped, say which. If
you could not verify something, say that rather than describing what would
probably happen. If you need to rename variables to match the context, do so. If
you need to separate large files into smaller ones, do so. A confident wrong report
is the single most expensive thing an agent can produce here, because it is
the one that gets believed.

---

## Completion Checklist

- Have you profiled the code?
- Have you tested the code?
- Are all code paths covered in tests?
- Can you improve the code further?
- Are variables and fields not named generic things?
- Are there dead code paths you must remove?
- Have you formatted your code correctly?
- Are you doing any 'negative' C++ code practices?
- Are you doing any 'negative' general code practices?

