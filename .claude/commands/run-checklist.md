---
description: Work through the completion checklist in docs/CODE_QUALITY.md before a pull request.
---

Work through [`docs/CODE_QUALITY.md`](../../docs/CODE_QUALITY.md) against the
current change, start to finish.

**Read that file first.** It is the checklist; this is only how to walk it. If
the two ever disagree, the file is right and this needs fixing.

## What "the current change" means

```sh
git status --short
git diff --stat
git diff
```

Everything uncommitted, plus anything on this branch that is not on `main`. If
the diff is large enough that you cannot hold it in your head, say so — the
checklist itself says a change too large to review in a few minutes is too
large, and that is a finding worth reporting before any of the rest.

## Section 1 — the mechanical gate

Run every command in that section. **Run all of them even after one fails**, and
report the failures together; stopping at the first turns one round trip into
five.

The `ci` preset goes last. It is the one most likely to fail on something
trivial, and you want the real failures first.

Two things worth noticing that a green run will not tell you:

- **A warning in one preset and not the other.** Usually it means something is
  only compiled on one path.
- **`just format` changing files.** Say which. A formatting-only diff mixed
  into a behavioural change is what makes a review hard to read.

## Sections 2 to 9 — the review

These need judgement, so answer them against the actual diff rather than in
general. For each section, either name the specific thing in this change that
the question applies to, or say the section does not apply and why.

"Checked" as a bare word is not an answer. If section 4 asks which thread
something runs on, the useful answer names the function and the thread.

Some of these are checkable rather than arguable, and those should be checked
rather than reasoned about:

- **New module?** Confirm it is in
  `mono.tools/architecture/expected_graph.json`, and that the architecture test
  passes with it.
- **New test file?** Confirm it has a `TEST_SUITE_ID`, and that
  `testrunner --list` shows it. A file without one is invisible to the cascade
  and will be silently skipped forever.
- **New command-line flag?** Run the program's `--help` and confirm it appears.
- **Changed something an `AGENTS.md` documents?** Re-read that file and confirm
  it is still true. If it is not, updating it is part of this change, not a
  follow-up.

**Section 8 is the one to slow down on.** It is last, it has no build step
behind it, and it is therefore the one that gets waved through. Read the diff
again specifically for it:

- Every new name — is any of them `data`, `info`, `manager`, `handler`,
  `value`, `temp`, `result`? Does anything with a unit carry it in the name?
- Every file you added or grew — can you say what it is for in one sentence?
- Every branch you added — is any of it now unreachable, and is there anything
  left over from an approach you abandoned?
- The two "negative practices" lists are specific and worth going through item
  by item on a diff of any size. Most of them compile and pass tests, which is
  exactly why they need a person.

## Section 10 — reporting

Then report, in this shape:

1. **Gate** — what passed, with numbers. "9/9 on dev, 6/6 on server, ci clean."
2. **Findings** — anything sections 2 to 9 turned up, most serious first. A
   finding is a specific thing in a specific file, not a category.
3. **Not run** — every step you skipped or could not run, and why. No GPU, a
   missing tool, a preset that would not configure. This section being empty is
   a claim; make sure it is true.
4. **Gaps** — what the change deliberately does not do. The checklist asks the
   pull request to say this, so work it out now.

## The rule that outranks the rest

Do not report a step as passing that you did not run. If something could not be
verified, say that instead of describing what would probably have happened.

A confident wrong report is the most expensive thing this whole checklist exists
to prevent, because it is the one that gets believed.
