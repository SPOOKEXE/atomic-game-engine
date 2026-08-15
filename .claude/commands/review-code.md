---
description: Review one area of the codebase against docs/CODE_QUALITY.md and fix what it finds, code or tests.
argument-hint: <area> [code|tests]
---

Review `$1` against [`docs/CODE_QUALITY.md`](../../docs/CODE_QUALITY.md), then
**fix what you find.** This command changes code. That is the whole difference
between it and the other two.

`/run-checklist` and `/pr-analysis` are both scoped to a diff and both end in a
report. Standing code has no diff - nobody is about to open a pull request
against `mono.engine/spatial`, so nothing ever walks it. This is the command for
that: pick an area, read it properly, improve it, prove the improvement.

`$2` narrows the pass to `code` or `tests`. With no second argument, do both,
code first - a test written against a signature you are about to change is work
thrown away.

## Resolving the area

`$1` is one of:

- a module - `ecs`, or `mono.engine/ecs`. Both mean the same directory. If the
  bare name is ambiguous, list the candidates and ask.
- a program or tool - `mono.client`, `mono.studio`, `mono.tools/docgen`.
- a single file, when you want a narrow pass.

Refuse `mono.vendor` and anything under it. It is submodules; a fix there
belongs upstream and a local edit is lost on the next update.

Refuse the whole repository. "Review everything" produces a diff nobody can
review, which is the failure `docs/CODE_QUALITY.md` opens by naming. If `$1` is
a directory holding several modules, say which ones and ask for one.

## Before judging anything

Reading the area is not optional and it is not `grep`. A finding produced
without understanding what the code is for is how a working invariant gets
"fixed" into a bug.

1. **The area's `AGENTS.md`, and the root one.** The module file carries the
   invariants - the things that are true here and would be broken by a
   reasonable-looking change. Half of what looks wrong in a well-written module
   is written down there as deliberate. If you are about to raise something,
   check first that the file does not already explain it.
2. **The public headers, then the sources, then the tests.** In that order. The
   headers are the surface the rest of the engine is holding on to, and they
   bound what you are allowed to change quietly.
3. **The call sites.** `codegraph explore "<the symbols>"` gives you the source
   plus who calls it, in one pass. Reach for it before `grep`, and reach for it
   *before* editing rather than after - the blast radius decides whether a
   rename is a rename or a breaking change.
4. **`mono.tools/architecture/expected_graph.json`** for what this module links
   and what links it.

Say, briefly, what the area is *for* before you say anything is wrong with it.
One or two sentences. If you cannot write them, you have not read enough to
review it.

## The code pass

The questions are in `docs/CODE_QUALITY.md` and this does not restate them -
two copies of a checklist drift and then nobody knows which is current. Work
sections 2 to 9 over the area rather than over a diff.

Weight them differently than a pull request would, though, because the
diff-shaped sections have nothing to bite on here:

- **§8 Craft, and the two negative-practice lists, are the point.** They are the
  sections with no build step behind them, which is precisely why standing code
  accumulates them. Go through the negative lists item by item - an owning raw
  pointer, a `string_view` outliving its owner, a `static` non-trivial global, a
  `catch (...)` with no comment, a magic number, a swallowed error, a `TODO`
  with no version. All of them compile and pass, and none of them will ever be
  found by anything except somebody looking.
- **§3 Correctness** on the paths tests do not reach. Empty input, first frame,
  the guarded division, the error path that leaves an object half-initialised.
- **§5 Performance is to report, not to act on.** A speculative optimisation
  with no measurement is a behaviour change bought with nothing. If the area has
  a `bench/` suite, `just bench <area>` is the sanctioned way to get a number,
  against the `bench` preset. Do not write a throwaway benchmark to justify a
  change, and do not describe an improvement you have not measured.
- **§2 Architecture** where it applies without a diff - a public header only
  this module uses, a vendor type that reached the surface, a second way to do
  something that already existed, an integer identifying something that leaves
  the process.

Two more that are specific to reviewing code nobody is currently changing:

- **Is anything here dead?** A function with no callers, a parameter nobody
  passes, an option that does nothing, a branch that cannot be reached. Confirm
  it with the call graph rather than by eye, then delete it. Git has it, and
  dead code is read as live by everybody who comes after - including the next
  model.
- **Does the module's `AGENTS.md` still tell the truth?** It is the one document
  in the area that no build step checks. An invariant written there that the
  code stopped honouring is worse than nothing, because it is believed.

## The tests pass

Coverage as a number is not the question. These are:

- **Does each test fail if the code is wrong?** This is checkable and it is the
  only one that matters, so check it rather than reading for it. Break the thing
  under test deliberately - flip a comparison, drop a clamp, return the wrong
  branch - run the suite, watch it go red, put the code back. A test that passes
  both ways is worse than no test, because it is believed. Report which tests
  you did this to, and put back **every** mutation you made.
- **Is the interesting case covered, or only the easy one?** Empty, one, many,
  wrong. The last is the one usually missing.
- **Is every branch reachable from a test?** Especially the error paths. An
  error path with no test has never run.
- **Does it test behaviour or implementation?** A test that has to change
  whenever the code is refactored is a cost with no benefit. Rewrite it against
  the public surface, or delete it.
- **`TEST_SUITE_ID` and `TEST_DEPENDS`.** A file without an id is invisible to
  the runner and has been silently skipped for however long it has existed -
  confirm with `just test-list`, which says what would run and why. Missing
  `TEST_DEPENDS` is quieter and just as bad: the suite stops re-running when the
  thing under it changes.
- **Is there a suite at all?** One file per public header is the default. The
  three deliberate exceptions are in `AGENTS.md` - a behaviourless handle
  covered where it is used, a program's own headers covered by the loop that
  drives them, and `Renderer.hpp`, which needs a GPU and is checked by running
  the client. **Do not add a mock renderer to close that gap on paper.**
  Anything else with no suite is a gap; write it.

Two things you may not do to make a test easier: widen a public header, and
weaken an assertion. The test can link the module's `src/` if it must - that is
what the private include directory is for.

## Fixing

Work the findings in ranked order, worst first. After each one, or each small
group of related ones, build and run the affected suites. A batch of twelve
edits verified once tells you something is broken and not which edit did it.

Three categories, and they are treated differently:

- **Behaviour-preserving** - a rename, a dead branch removed, a `unique_ptr`
  replacing a `new`/`delete` pair, a missing test, a comment that explains the
  decision instead of restating the line. Do these. That is what the command is
  for.
- **Behaviour-changing** - a bug fixed, a guard added, an error path that now
  reports instead of continuing. Do these too, but say so loudly in the report
  and write the test that fails without the fix, in the same pass. A fix with no
  failing test behind it is a claim.
- **Design-changing** - splitting a file, moving something across a module
  boundary, changing a public signature, anything touching a tier or a layer.
  **Propose, do not apply.** Something depends on the current shape, and the
  decision is not yours to make inside a review. Write it up in the report with
  what it would cost.

Bound the change. `docs/CODE_QUALITY.md` treats a diff too large to review in a
few minutes as a finding in its own right, and that applies to the diff *this
command produces*. When you reach that size, stop, report what is done and what
is left, and let the rest be a second run. A review that lands four hundred
lines of unreviewable improvement has produced a problem, not fixed one.

You may not edit `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `README.md`, or
anything under `docs/` named `CODE_*.md`. The root `AGENTS.md` says so. If the
review finds one of them false - most likely a module `AGENTS.md` whose
invariant the code no longer honours - report it and let it be decided. That is
an important change, and important changes are the stated exception, verified
rather than assumed.

## Verifying

Everything you changed has to be proven, and the mechanical gate in
`docs/CODE_QUALITY.md` §1 is how. Run all of it, not the convenient half:

```sh
just format
cmake --preset dev    && cmake --build .cache/build/dev -j
cmake --preset server && cmake --build .cache/build/server -j
just test-all
just check-server-is-headless
cmake --preset ci && cmake --build .cache/build/ci -j
```

`just test-all` rather than `just test`, even though the cascade exists - the
cascade is an optimisation for the inner loop, and this is not the inner loop.
Both presets, because a `client`-tier edge picked up by accident only fails in
the server-only configure.

If you touched a public header, `just docs-check` as well. It is at zero gaps
and the point of a check at zero is that it stays there - a new public entity
with no comment, or a `@param` naming an argument you renamed, both fail it.

Run everything even after one step fails, and report the failures together.
Stopping at the first turns one round trip into five.

## Reporting

1. **The area** - what it is for, in the sentences you wrote before starting,
   and what you actually read.
2. **Changed** - every edit, as `<file>:<line> - <what> - <why>`. Grouped by the
   three categories above, with the behaviour-changing ones first and their
   tests named.
3. **Found and not changed** - the design-changing proposals, and anything you
   were not confident enough to touch. Each with what it would cost and what it
   would buy. This section being short is a claim about the area; make sure it
   is true.
4. **Verification** - what passed, with numbers. Which tests you mutated to
   check they fail, and confirmation that every mutation is out.
5. **Not run** - every step you could not run, and why. No GPU, a missing tool,
   a preset that would not configure.

## The rules that outrank the rest

**Do not change what you do not understand.** In a codebase with a deliberate
layer stack, the line that looks wrong is frequently the line that is load-
bearing, and the comment explaining why is the thing that was missing - not the
code. When the honest position is "this looks odd and I cannot tell if it is
deliberate", that is a finding worth reporting, and it is a better one than a
confident edit.

**Do not report a step as passing that you did not run.** A confident wrong
report is the most expensive thing the checklist exists to prevent, and it costs
more from this command than from the others, because this one has already edited
the code by the time it says so.
