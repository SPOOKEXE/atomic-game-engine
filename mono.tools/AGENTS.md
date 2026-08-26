# mono.tools - module invariants

The developer-facing tools. Nothing here ships in a game binary.

| Directory | What it is |
|---|---|
| `testrunner/` | the selective test runner - a C++ library, a thin main, and its own tests |
| `architecture/` | a CMake script, and the checked-in expectation it reads |
| `sourcecheck/` | the four architecture rules that are visible in source text rather than in the target graph |
| `docgen/` | the documentation filter, and the `docs` target it feeds - see its own `AGENTS.md` |

## No scripting runtime

The prerequisite list for this repository is CMake, Ninja, a C++20 compiler and
`glslc`. Adding a language for tooling makes it five, for programs that produce
nothing a game needs - and the audience this engine is for is people who know
games better than build systems.

A tool is C++ when it is a program, and CMake when its input is CMake's own
output. If you find yourself wanting a scripting language here, the question to
answer first is which of those two the tool actually is.

`docgen` is the case that tests the rule. A comment filter is the most natural
thing in the world to write in Python, and it is C++ here - which is why the
interesting half of it is a library with a Catch2 suite rather than a script
nothing can test. Its `RunDoxygen.cmake` is the deliberate exception, and its
`AGENTS.md` argues the case.

## The runner is a build artifact, and that is fine

The obvious objection is bootstrap: a C++ tool has to be built before it can
check the build. It does not apply. The runner only ever runs *after* a
successful build, so being one costs it nothing.

It does apply to the architecture check, which is why that one is a CMake script
- it runs against a tree that has been configured but not built, and CMake has
the JSON reader already.

## The runner must be able to see its own tests

This is the reason `testrunner/` is a library plus a thin main rather than one
`main.cpp`. Its tests are ordinary Catch2 suites with `TEST_SUITE_ID`, so the
runner discovers them, hashes them, and re-runs them through the same cascade as
everything else.

A test runner whose own tests are invisible to it is one whose bugs hide other
bugs - by reporting success. If you add a tool here, give it a Catch2 suite, not
a bespoke harness.

## `smart-tests.txt` is text

Tab-separated, one line per suite, with a version header. A person can read it,
diff it, and delete a line to force a re-run. If it needs a structure that text
cannot carry, the format was wrong - do not put JSON behind a `.txt`.

The version header is load-bearing: signatures from a different version were
computed differently, so a cache that does not match is discarded rather than
misread. Misreading it would skip suites on the strength of numbers that do not
mean the same thing.

## Derived file sets, hand-declared identifiers

Suite identifiers and their dependencies are hand-written, because a person
knows what a test is about. The set of *files* a suite depends on is derived
from the compiler's own dependency data, because a hand-written list goes stale
silently - and a stale list means a skipped test that should have run, which is
the worst failure mode a runner has.

Never add a way to declare files by hand. Where the derivation fails - no Ninja
database, a different generator - the runner warns and falls back to hashing the
source alone. It says so rather than narrowing in silence.

## `sourcecheck` is the source-text half of the architecture check

`CheckTargetGraph.cmake` reads CMake's output and can therefore see the module
set, the tiers, the link sets and the layer heights. It cannot see a member's
type, a header's includers, or an argument reaching a writer, and those are what
root `AGENTS.md` rules 2, 3 and 4 and `docs/CODE_ARCH.md` §3 are about. So there
are two tools rather than one, and they are C++ and CMake for the reason above:
one's input is CMake's output and the other's is C++.

**It is a scanner, not a front end, and the difference is the whole design.** A
tool that resolved types properly would need the build's include paths, which
means it could only run after a configure - and the point of both checks is that
they run against a tree nobody has built. Every rule is therefore a heuristic
over declarations, `docs/CODE_ARCH.md` §11.1 tables what each one catches and
misses, and `Rules.hpp` repeats it at each function.

**A rule is switched off with `// arch-waiver <rule>: <reason>` above the
declaration**, and the reason is load-bearing: a waiver with nothing after the
colon is reported rather than honoured. Do not add a suppression list anywhere -
a list drifts away from the code it names and nobody notices, and a comment dies
with the declaration it sits on.

**Its fixtures are not optional.** Three of the four rules find nothing in this
repository, so nothing else would notice if the scanner stopped reading. The
fixture trees under `tests/fixtures/` are inputs that must fail with a named
sentence, plus one that must pass, and `tests/fixtures/README.md` says why. That
is the same argument `architecture/tests/README.md` makes, one tool along.

## The architecture check is not the tier enforcer

`mono_check_all_tiers` in `MonoLibrary.cmake` enforces the tier rule at
configure time and fails the build with the offending edge named. That is the
enforcement.

`CheckTargetGraph.cmake` does the other half: it compares the graph to
`expected_graph.json`, so that an architectural change shows up as a diff
somebody reviews rather than only in a build log.

`expected_graph.json` is an architectural document, not a lock file to be
regenerated when the build complains. Do not add a `--update` flag.

## Not here yet

`bindgen`, `assetc`, `scenemerge`, `tsluau` and `mcp` are named in
`repo_layout.md` §3 and do not exist. Each arrives with the thing it serves.
