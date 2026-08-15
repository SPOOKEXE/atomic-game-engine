# Contributing

Thanks for looking. This engine is being built mostly by and for people coming
from the Roblox community, and the assumption throughout is that you know games
better than you know C++ build systems.

---

## Get it building

You need:

| Tool | Version | Why |
|---|---|---|
| CMake | 3.24+ | the build |
| Ninja | any recent | the generator every preset uses |
| A C++20 compiler | GCC 13+, Clang 16+, MSVC 19.3+ | |
| `clang-format` | 18+ | `just format`; optional, but a pull request without it says so |
| `just` | optional | the entry point; raw CMake works too |
| Python 3 | 3.8+ | **`just setup` only** - see below |

Nothing in the build or the test suite needs a scripting runtime: the test
runner is C++ and the architecture check is a CMake script.

`just setup` is the exception, and only for shaderc. It pins glslang,
SPIRV-Tools and SPIRV-Headers in its own `DEPS` file rather than as submodules,
so `git submodule update --recursive` clones shaderc and leaves `third_party/`
empty. `utils/git-sync-deps` fills it, and that script is Python. You need it
once, on a fresh clone.

You do **not** need `glslc` installed. It is built from `mono.vendor/shaderc`,
which is what makes the SPIR-V reproducible between machines. Configure with
`-DMONO_VENDORED_GLSLC=OFF` to use one from your PATH instead and skip building
glslang - faster first build, at the cost of whatever version you happen to
have.

On Linux you will also want the usual X11/Wayland development packages - SDL
picks its backends at configure time and tells you what it found.

```sh
git clone --recurse-submodules <url>
cd atomic-game-engine

just setup      # or: git submodule update --init --recursive --depth 1
just build
just run --stats
```

Without `just`:

```sh
cmake --preset dev
cmake --build .cache/build/dev
./.cache/build/dev/client/client --stats
```

Everything derived lands in `.cache/`. Deleting it costs you a rebuild and
nothing else.

---

## Run the tests

```sh
just test        # only what your change could have affected
just test-all    # everything
```

The first one is not a guess. Each suite's signature is a hash of its source,
every header that source includes, and the signatures of the suites it declares
a dependency on - so changing a header at the bottom of the stack re-runs
everything above it and nothing else. It prints what it skipped.

`ctest` from a build directory also works and always runs everything.

Tests live with the thing they test - `mono.engine/<module>/tests/`,
`mono.client/tests/`, `mono.server/tests/`, `mono.tools/tests/`. There is no
central test directory.

---

## Try it

```sh
just run --stats --graph
```

- **F3** - frame rate, and the shape of the last twenty seconds. The minimum
  is the interesting number.
- **F5** - the frame graph. **F6**/**F7** move between views: the flamegraph,
  time by category, per-system cost, and the metrics counters.
- **Esc** - quit.

`--uncapped` unpins the frame from your display, which is the only way the
numbers mean anything. `client --help` lists the rest.

The server is headless and has no window at all:

```sh
just host --ticks 300 --entities 20000
```

`just check-server-is-headless` configures with no client, and fails if the
staged `server/` directory has grown a `shaders/` folder. That is the tier
split proved rather than asserted.

[RUNNING.md](RUNNING.md) has the rest: every flag on every program, how to run
one suite directly, how to attach Tracy, and why `--uncapped` is not optional
when you care about the numbers.

---

## Before you open a pull request

**Work through [`docs/CODE_QUALITY.md`](docs/CODE_QUALITY.md).** That is the
completion checklist - the mechanical gate first, then the review questions,
then what the pull request itself has to say.

It lives there rather than here on purpose. A checklist copied into two files
is two checklists, and within a few months nobody knows which one is current.

Working with Claude Code, `/run-checklist` walks the whole thing and reports
what it could not verify rather than skipping it quietly. `/new-module` walks
through adding an engine module without missing one of the six things that have
to line up. Both live in [`.claude/`](.claude/README.md) and are checked in.

Two things the checklist cannot check for you:

**Keep it small.** A change a reviewer can hold in their head gets reviewed
properly; one that cannot gets approved instead. Several small pull requests
beat one large one, even when the large one is better work.

**Say what you did not do.** A pull request that names its own gaps is easier
to trust than one that does not mention any.

---

## On AI

This project uses AI models heavily and says so on the front page. That is a
deliberate choice about pace, and it comes with one condition, which is not
negotiable:

> **You are responsible for every line you submit.** You must be able to
> explain it, defend it in review, and maintain it. "The model wrote it" is not
> an answer to a review comment.

Practically, that means: read what it produced before you send it. If you do
not understand a section, either understand it or take it out. A change nobody
can explain is a change nobody can fix, and this is a codebase intended to last.

Use AI freely for the parts where it is genuinely good - boilerplate,
mechanical refactors, working out an unfamiliar API, writing the test you know
you should write. Be much more careful with it on architecture, on anything
touching the untrusted parsing boundaries, and on anything where "looks right"
and "is right" are hard to tell apart.

Always test your code, always benchmark and profile your code, always check if you need to rename variables and always check if you need to split code from one file to multiple.

---

## Reporting a bug

Say what you did, what happened, and what you expected. A frame from `--stats`
or `--graph` is worth several paragraphs. Include your GPU backend - the F3
panel prints it.

For anything touching the sandbox, the game file reader or the network path,
see `SECURITY.md` before opening a public issue.

---

## Completion Checklist

**It lives in [`docs/CODE_QUALITY.md`](docs/CODE_QUALITY.md).** A checklist
copied into two files is two checklists, and within a few months nobody knows
which one is current - so this section is a pointer, not a copy.

The short version, and where each part is answered in full:

| Ask yourself | Section |
|---|---|
| Have you profiled the code? | [5 · Performance](docs/CODE_QUALITY.md#5--performance) |
| Have you tested the code? | [6 · Tests](docs/CODE_QUALITY.md#6--tests) |
| Are all code paths covered in tests? | [6 · Tests](docs/CODE_QUALITY.md#6--tests) |
| Can you improve the code further? | [8 · Craft](docs/CODE_QUALITY.md#8--craft) |
| Are variables and fields not named generic things? | [8 · Craft](docs/CODE_QUALITY.md#8--craft) |
| Are there dead code paths you must remove? | [8 · Craft](docs/CODE_QUALITY.md#8--craft) |
| Have you formatted your code correctly? | [1 · The mechanical gate](docs/CODE_QUALITY.md#1--the-mechanical-gate) |
| Any 'negative' C++ practices? | [8 · Craft](docs/CODE_QUALITY.md#8--craft) - named, one by one |
| Any 'negative' general practices? | [8 · Craft](docs/CODE_QUALITY.md#8--craft) - likewise |

The full checklist also covers the layer stack, the untrusted parsing
boundaries, thread affinity, and what the pull request itself has to say. Those
are the ones a reviewer cannot check for you afterwards.

Working with Claude Code, `/run-checklist` walks all of it.

---

## Licence

MPL-2.0. By contributing you agree your contribution is licensed under it.
