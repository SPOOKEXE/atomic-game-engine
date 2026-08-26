# Fixtures for the source-text architecture rules

`mono.tools/architecture/tests/README.md` says why the architecture check is
checked: it walks an expectation, and an expectation it fails to parse walks as
zero entries and reports success. **`sourcecheck` has the same shape and a wider
mouth.** It walks a tree of C++ as text, and a tree it fails to read - a member
declaration its scanner does not recognise, a comment marker it does not see -
scans as zero declarations and reports success. Three of its four rules find
nothing at all in this repository today, so nothing else here would notice if
they stopped working.

So each rule has inputs that must fail and inputs that must pass.

Each directory is a `tree/` standing in for a repository, and an `expect` file
naming a phrase the tool's output must contain. `just source-check` runs every
fixture first and the real tree afterwards - a green scan of the repository
means nothing until the scanner has been shown to bite.

| Fixture | What it holds |
|---|---|
| `ecs-copy` | a long-lived object holding a registered component |
| `ecs-copy-enum` | the same, holding a component's companion enumeration |
| `ecs-copy-waived` | the first one with a waiver and a reason. **Must pass** |
| `ecs-copy-no-reason` | a waiver with nothing after the colon, which is not honoured |
| `world-pointer` | a pointer two field hops inside a type marked `arch-crossing` |
| `world-pointer-view` | a `std::span` in the same place, which is a pointer with a nicer name |
| `world-pointer-clean` | a `std::vector` in the same place. **Must pass** |
| `name-id` | `WriteUInt32(name.Id())` |
| `name-id-raw` | `WriteRaw(&name, sizeof(Name))`, the object-representation write |
| `name-id-read` | `Name::FromId(reader.ReadUInt32())`, the same rule on the way back in |
| `name-id-clean` | `WriteName` and `Text()`. **Must pass** |
| `public-header` | a header under `include/` that only its own module includes |
| `clean` | one of each done right. `expect` is empty, so it must pass - without it a tool that failed on everything would go green here |

**The trees are `.hpp` only, and that is not a style choice.**
`mono_add_tests` globs `tests/*.cpp` recursively, so a `.cpp` under here would
be compiled into this tool's own test binary - a fixture full of deliberate
violations would have to be valid, linkable C++ first. Headers are inert to that
glob. `sourcecheck` reads a header the same way it reads a translation unit, so
nothing is lost.

`Scan` skips any directory named `fixtures` for the matching reason: a scan of
the repository that contains these would otherwise report every violation in
them, and the tool would fail on its own test data.

## What the runner asserts

- `expect` empty: no open finding of any kind, and exit 0.
- `expect` non-empty: the phrase appears in the output, **and** the exit status
  agrees with `Gating` - non-zero when the open finding is on `ecs-copy`,
  `world-pointer` or `name-id`, and zero when it is only on `public-header`,
  which reports and never gates.

That last clause is the one worth keeping. Without it a fixture would pass by
printing the right sentence while the build sailed on, which is the failure
`docs/CODE_ARCH.md` §11 describes for a check that is broad and ignored.
