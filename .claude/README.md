# .claude/

Skills and workflows for agents working in this repository. Checked in, so
every contributor gets the same ones rather than each person's model inventing
its own conventions.

```
.claude/
├─ skills/
│  ├─ grug/          blunt, practical takes on a design question
│  └─ ste-writing/   rewriting prose into Simplified Technical English
└─ commands/
   ├─ run-checklist.md  the completion checklist, before a pull request
   ├─ pr-analysis.md    reading a change, and what it should have touched
   ├─ review-code.md    reviewing an area with no diff, and fixing it
   └─ new-module.md     scaffolding an engine module correctly
```

These sit under the rules in [`AGENTS.md`](../AGENTS.md), not beside them. A
skill changes how something is said or approached; it does not suspend the layer
stack, the tier rule, or your responsibility for the code.

---

## The skills

### `grug`

Invoke with `/grug`. Gives the cave-developer take: complexity is the enemy,
most abstractions are not worth it, the boring option usually wins.

Worth reaching for when a design is growing and you cannot articulate why it
feels wrong. It is a good counterweight in a codebase like this one, which has
a deliberately layered architecture and is therefore permanently one bad
afternoon away from over-engineering.

It is a voice, not an oracle. Grug will tell you the tier system is complexity
demon. Grug is half right and the answer is still to keep it, because the thing
it prevents is worse. Use the take, then decide.

### `ste-writing`

Invoke with `/ste`. Rewrites prose into ASD-STE100 Simplified Technical
English: short sentences, active voice, one name per thing, no marketing
adjectives.

**Where it belongs here:** error messages, log lines, `--help` text, release
notes, commit messages, and anything procedural - the "Get it building" section
of `CONTRIBUTING.md`, the steps in `check.md`. Anything a person follows under
pressure.

**Where it does not:** `AGENTS.md` files, `docs/CODE_QUALITY.md`, and the
comments that explain a decision. STE strips voice on purpose, and those
documents are doing something voice is required for - they argue. "Reflecting
the velocity without clamping the position lets an entity that overshot sit
outside the box flipping every tick" is not STE-compliant and should not be.
Flattening it into "the code clamps the position" deletes the reason, which was
the entire content.

The skill says this itself: *"It cannot make a hollow paragraph true."* The
inverse also holds - it can make a true paragraph useless, if the truth was in
the argument.

---

## The commands

### `/run-checklist`

Walks `docs/CODE_QUALITY.md` against the current diff: the mechanical gate
first - format, both presets, every test, the architecture check,
warnings-as-errors - then the review questions, then what the pull request
itself has to say.

The command does not restate the checklist, it points at it. There was briefly
a separate `/check` covering only the mechanical half; it was deleted rather
than kept alongside this, because two commands for one job is the debt
`AGENTS.md` warns about and both would have grown their own copy of the steps.

For "did I break anything" mid-change, `just test` is faster than either.

### `/pr-analysis [pr-number|base-ref]`

Reads a change and reports five things: what it adds, removes and edits; the
areas it touches; the lower-level places it should have reached and did not;
the higher-level ones; and a ranked list of what to change or clarify. With no
argument it takes this branch against `main` plus the working tree; given a
number it takes that pull request through `gh`.

It reads and does not run - deliberately. `/run-checklist` already owns the
mechanical gate, and a second command configuring both presets would be two
copies of the same steps within a month. The two are complements: this one asks
whether the change is the right shape, that one asks whether it holds up.

The middle two sections are the reason it exists. A diff summary is cheap and a
review of what a diff contains only ever finds what somebody wrote down. The
costly mistakes here are absences - a new type that the future Luau bindings
will have to expose under a name nobody chose, a `shared` module that quietly
grew a `client` edge, a flag that works but never reached `--help`. Those all
build and pass.

Splitting the absences by direction is the part worth keeping. Downward is
mostly checkable - tier edges, `expected_graph.json`, `TEST_SUITE_ID`, declared
flags. Upward is mostly not, because the scripting layer it argues about does
not exist yet, and that is exactly when the decisions are cheap.

### `/review-code <area> [code|tests]`

Takes a module, a program or a file, reads it properly, works
`docs/CODE_QUALITY.md` over it, and then **fixes what it finds.** The second
argument narrows it to one pass; with no argument it does both, code first.

It exists because the other two are scoped to a diff and standing code does not
have one. Nobody is about to open a pull request against `mono.engine/spatial`,
so nothing ever walks it, and the sections of the checklist with no build step
behind them - §8 Craft and the two negative-practice lists - are exactly the
ones that accumulate when nobody is looking. Every item on those lists compiles
and passes tests. That is why they are on a list.

It is the only command here that edits. Three consequences worth knowing before
running it:

- **It only applies what it can defend.** Behaviour-preserving fixes and bug
  fixes with a failing test behind them, yes. Splitting a file, moving something
  across a module boundary, changing a public signature - proposed and left
  alone, because something already depends on that shape and a review is the
  wrong place to decide.
- **It runs the full gate afterwards**, both presets and `test-all` rather than
  the cascade. The cascade is for the inner loop; a command that has already
  changed the code owes more than that.
- **It stops when the diff gets too large to review.** The checklist treats that
  as a finding, and there is no reason the rule stops applying to a diff this
  command produced. The remainder is a second run.

The performance section is deliberately report-only. A speculative optimisation
with no measurement is a behaviour change bought with nothing, and `just bench`
against the `bench` preset is the way to get a number - not a throwaway harness
written to justify a change already made.

### `/new-module <name>`

Adding a module means six things lining up - directory shape, tier, layer
position, the `add_subdirectory`, the entry in
`mono.tools/architecture/expected_graph.json`, and an `AGENTS.md` worth
reading. Missing any one of them produces something that builds today and is
wrong later. This walks through all six and asks the questions that should be
answered before any of it.

---

## Adding one

**A skill** is a persona or a way of working - something reusable across tasks.
`.claude/skills/<name>/SKILL.md`, with `name` and `description` frontmatter. The
description is what decides whether it triggers, so write it as the phrases
somebody would actually type.

**A command** is a procedure - a named sequence for a task this repository does
often. `.claude/commands/<name>.md`, with a `description` and optionally an
`argument-hint`.

Two things to get right in either:

- **Say why, not just what.** A step whose reason is not written down is one
  somebody will skip when it is inconvenient. Most of `new-module.md` is
  reasons.
- **Keep it true.** These go stale exactly like a comment does, and a workflow
  that tells an agent to run a recipe that no longer exists is worse than
  nothing. If you rename a `just` recipe, grep here.

Personal settings belong in `.claude/settings.local.json`, which is gitignored.
Anything checked in is a decision for everybody.
