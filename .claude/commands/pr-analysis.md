---
description: Summarise a change, name the areas it touches, and find the lower-level and higher-level places it should have touched and did not.
argument-hint: [pr-number|base-ref]
---

Analyse the change described by `$ARGUMENTS` and report on it. Five sections,
in the order below.

This command **reads and does not run**. No configure, no build, no `ctest`.
`/run-checklist` owns the mechanical gate and running it twice is how two
commands grow two copies of the same steps. The consequence is that everything
here is a claim about the diff, not a verified fact about the binary - say so
where it matters, and never describe a build result you did not produce.

The useful half of this command is sections 3 and 4. Anyone can list what a
diff changed; the expensive mistakes in this repository are the files a change
*should* have touched and did not, because those compile, pass, and are wrong
later.

## Resolving the change

```sh
git status --short
git diff --stat main...HEAD
git diff main...HEAD
git diff                      # working tree, on top of the above
```

With no argument that is the scope: everything on this branch that is not on
`main`, plus everything uncommitted. Most work here is uncommitted for a while,
so leaving the working tree out would usually analyse nothing.

- `$1` is all digits - a pull request number. `gh pr diff $1` and
  `gh pr view $1`, and ignore the working tree; you are analysing what is on
  GitHub, not what is on this disk. Say which you used.
- `$1` is anything else - a base ref. `git diff $1...HEAD`.

Read the diff itself, not only `--stat`. If it is too large to hold in your
head, say so before anything else - `docs/CODE_QUALITY.md` treats that as a
finding in its own right, and it changes how much the rest of this report is
worth.

## 1 - What it does

Plain text, no table. Three lists, and any of them may be empty:

- **Adds** - new files, new public headers, new flags, new modules, new
  behaviour.
- **Removes** - deleted files, deleted symbols, behaviour that no longer
  happens, flags that no longer exist.
- **Edits** - changed behaviour in something that already existed.

One line each, in the vocabulary of the codebase. "Adds
`ecs::Store::EachParallel`, a blocking parallel iterate with a 4096 grain" is
useful. "Improves ECS performance" is not, and cannot be checked by the person
reading it.

Sort by consequence, not by path. A one-line change to a tier declaration
outranks four hundred lines of new tests.

## 2 - Affected areas

For each `mono.X` folder the diff touches, name it, and say what it is now on
the hook for. Include areas the diff touches indirectly - a change to
`mono.engine/core` is a change to everything, because everything links it.

Anchor the blast radius rather than guessing at it:
`mono.tools/architecture/expected_graph.json` has each module's tier and the
transitive closure of what it links. Reading it backwards gives you the
dependents.

Then state, explicitly:

- Which **tiers** are involved - `shared`, `client`, `server`. A `shared`
  module that grew a dependency on a `client` one is the single most important
  thing this section can find, and the configure-time tier check will catch it,
  but only under a preset somebody remembers to run.
- Whether the diff crosses a **layer** downward. A layer may see every layer
  below it and none above.
- Which **`AGENTS.md`** files cover the touched code. List their paths. Every
  one of them is a file you now have to read, because its invariants are what
  the diff is being judged against.

## 3 - Lower-level, and missing

Places the change should have reached *downward* - the engine, the build, the
command line. Report only what this specific diff implies; a checklist item
that does not apply should be left out, not answered "n/a".

Work through these, and name the file and symbol for anything you raise:

- **The command line.** New behaviour that a user cannot reach, or reaches
  through a flag that is not declared in `core::Arguments`. Flags there are
  declared rather than scanned for, so `--help` is generated from the
  declaration - a flag missing from `--help` is a flag that was not declared,
  not a documentation gap. A flag accepted and ignored is worse than one that
  errors; if it must exist before the feature does, it warns, like `--script`
  in `docs/DEFERRED.md` D00001.
- **Names crossing a boundary.** Anything reaching a save file, a wire format
  or a manifest and identified by an integer. `Name::Id()` is a dense counter
  valid inside one process; serialising it undoes the whole type. Conversely, a
  hot inner comparison on a `std::string` that should have interned once.
- **Storage the ECS should own.** A new private vector, cache or dirty flag
  holding data another module also reads. Two copies of one fact drift the
  first time one is written inside a branch.
- **World boundaries.** A pointer or reference in anything crossing between
  worlds. This is what keeps thread-per-world and process-per-world
  interchangeable, and one shared pointer added because "it is only threads
  today" ends the process option invisibly.
- **Parallelism.** New use of `Store::EachParallel` or `Jobs::For` with a grain
  or threshold and no measured number in a comment next to it - below the
  crossover, parallel is slower, and the crossover is higher than it looks. Any
  result that could land a tick later than it did before; that is a desync, not
  a latency change.
- **The architecture graph.** A new module, a new link, or a new build option
  that is not reflected in `mono.tools/architecture/expected_graph.json`,
  including a missing `requires` - without it the check cannot tell "deleted"
  from "not built under this preset".
- **Tests.** Every new or changed public header, against the suite that covers
  it in that module's own `tests/`. A new test file with no `TEST_SUITE_ID` is
  invisible to the cascade and will be skipped forever. The deliberate
  exceptions are in `AGENTS.md`: shared suites for small value types, handles
  covered where they are used, and `Renderer.hpp`, which needs a GPU and is
  checked by running the client. Do not propose a mock renderer.
- **The server staying headless.** Anything that could pull render, input or a
  shader into the `server` tier. The property is "does not contain one", not
  "does not start one".
- **Tracy.** A new per-tick hot path with no zone, when the change is one whose
  cost anybody will later want to see in the flamegraph.

## 4 - Higher-level, and missing

Places the change should have reached *upward* - toward the person writing a
script or a game against this engine, and the developer reading the docs. This
is the section most likely to be skipped, because none of it fails a build.

The engine has no VM yet; Luau and TypeScript arrive in ROADMAP v0.5–v0.6, and
the bindings manifest with them. That is exactly why this matters now: a name
or a shape that ships today is one the binding layer has to either expose or
rename later, and by then something depends on it.

- **The binding surface.** Any new public type, component or method that a
  script will eventually name. Is the name the one a scripter should type? Is
  it a component (data the ECS owns, visible to script) or a private
  implementation detail, and is the diff clear about which?
- **Blocking.** A new call that a script would have to wait on. ROADMAP v0.2
  commits to asynchronous and synchronous methods so scripts are not blocked -
  a new synchronous-only path is a decision to make now, not to discover when
  the VM lands.
- **One world assumed.** Anything written as though there is a single world or
  a single simulation. Universe-and-worlds is v0.2, and code that assumed one
  is the expensive kind to unpick.
- **Demos.** Whether the change is something `demo_1world_scene` or the
  two-world demo should show, or something that breaks them.
- **Docs a person actually opens.** `RUNNING.md` for any changed command line -
  it is the reference, and a stale command line there is a contributor's first
  five minutes. `README.md` for anything user-visible. `CONTRIBUTING.md` for
  anything that changes how you build or test. The module's own `AGENTS.md` for
  any invariant this diff made false: updating it is part of the change, not a
  follow-up.
- **`ROADMAP.md`.** Which item this advances, and whether it is now finished or
  only partly. A partly-done item must be split per the editing rules at the
  top of that file, keeping the done half ticked and the rest as its own line.
  Anything consciously not done goes in `docs/DEFERRED.md` as a new `D00000`
  entry at the front, not in `ROADMAP.md`.
- **Words a user reads under pressure.** New error messages, log lines and
  `--help` text. These are `/ste` territory: short, active, one name per thing.
  An error that does not say what to do next is a bug report you will receive
  later.

## 5 - Change or clarify

A ranked list, worst first. Each entry is one specific thing:

```
<file>:<line> - <what is wrong or unclear> - <why it matters> - <what to do>
```

A category is not an entry. "Naming could be better" is not actionable;
"`mono.engine/ecs/src/Store.cpp:212` - `data` holds the pending despawn set,
and the name says nothing" is.

Split it in two:

- **Should change** - you are confident, and the reason is stated.
- **Should clarify** - the diff is ambiguous and the right answer depends on
  intent you cannot read from it. Say what the two readings are and what each
  would imply.

### Asking

Where a clarification would change the analysis itself - a tier choice, whether
something is meant to be script-visible, whether an omission is deferred or
forgotten - ask, rather than reporting both branches at length. Ask once, in a
single batch, after you have read the diff and before you write sections 3 to 5.

Where it would not change the analysis, do not ask. State the assumption in the
report and carry on. A question whose answer changes nothing costs a round trip
and buys a sentence.

## The rule that outranks the rest

Distinguish, in the wording, between:

- what you read in the diff,
- what you inferred from it,
- and what you did not look at.

Sections 3 and 4 are the places to get this right, because "the change does not
handle X" and "I did not check whether the change handles X" read identically
and are not the same claim. The second one is fine; pretending it was the first
is what makes a report worse than no report.
