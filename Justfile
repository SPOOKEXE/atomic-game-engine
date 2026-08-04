# The engine developer's entry point. The only thing that knows how to drive
# CMake, the submodules and the test runner together.
#
# Game developers use the `atomic` CLI instead, and it never invokes CMake.

# The preset every recipe works against, and the directory it produces.
#
# Override it on the command line, before the recipe name:
#
#   just preset=release build
#   just preset=release run --stats
#   just preset=server test
#
# Not as a recipe parameter. `build` is derived from `preset` at parse time and
# a parameter cannot reach it, so `just test server` used to configure the
# server preset and then run the *dev* preset's binaries — a mismatch nothing
# reported. One override, applied before anything is derived, is the only form
# that cannot go wrong that way.
preset := "dev"
build  := ".cache/build/" + preset

_default:
    @just --list --unsorted

# Submodules, and anything else a fresh clone needs before it can configure.
setup:
    git submodule update --init --recursive --depth 1
    # shaderc pins glslang, SPIRV-Tools and SPIRV-Headers in its own DEPS file
    # rather than as submodules, so the line above clones shaderc and leaves
    # third_party/ empty. Without this the client configure stops in
    # MonoVendor.cmake pointing back here.
    python3 mono.vendor/shaderc/utils/git-sync-deps
    @echo "vendors ready"

# Configure one preset. Safe to re-run; CMake reuses the cache.
configure:
    cmake --preset {{preset}}

# Build everything, or one target. `just build engine_ecs` builds one module.
build target="":
    cmake --preset {{preset}} > /dev/null
    cmake --build --preset {{preset}} {{ if target == "" { "" } else { "--target " + target } }}

# One program and only its dependencies.
client: (build "client")
server: (build "server")
cdn: (build "cdn")
studio: (build "studio")

# Only the suites a change could have affected, by cascading signature hash.
test: build
    ./{{build}}/tools/testrunner --build {{build}}

# Every test, whatever changed.
test-all: build
    ./{{build}}/tools/testrunner --build {{build}} --all

# What the runner would run, and why, without running anything.
test-list: build
    ./{{build}}/tools/testrunner --build {{build}} --list

# Measure the benchmark suites a change could have affected.
#
# **The same selection as `just test`, over `bench/` instead of `tests/`.** A
# benchmark declares `TEST_SUITE_ID` and gets a cascading signature exactly as a
# test does, so a change at the bottom of the stack re-measures everything above
# it and nothing else. That matters more here than for tests: a test suite costs
# milliseconds and a benchmark suite costs seconds by design, and running all of
# them on every change is how a benchmark suite stops being run at all.
#
# Against the `bench` preset, which optimises. A debug build measures the debug
# build, and the danger is not that it is slower — it is that the *ratios*
# between two implementations invert.
#
# A number is reported, never enforced. A laptop on battery and a machine with a
# compile going both swing further than most real regressions, so `just bench`
# does not fail on a slow figure. Read it, then decide.
bench *args:
    cmake --preset bench > /dev/null
    cmake --build --preset bench
    ./.cache/build/bench/tools/benchrunner --build .cache/build/bench {{args}}

# Every benchmark, whatever changed.
bench-all *args: (bench "--all" args)

# Make what was just measured the numbers everything is compared against.
#
# Do this on a quiet machine and say so in the commit. A baseline taken while
# something else was compiling makes every later run look like an improvement.
bench-accept *args: (bench "--all" "--accept" args)

# How much of the repository is code, comment and blank, as markdown.
#
# `just linecount` walks everything except mono.vendor and the dot-directories;
# `just linecount mono.engine/render --files` narrows it and lists every file.
# Redirect it somewhere to keep it: the tool writes to stdout and nothing else,
# so a report that gets checked in is one somebody decided to check in.
linecount *args: (build "linecount")
    @./{{build}}/tools/linecount {{args}}

# The architecture test on its own — the target graph against the expectation.
# Needs a configure, not a build: it reads what CMake emitted.
test-architecture:
    cmake --preset {{preset}} > /dev/null
    cmake -DGRAPH={{build}}/target-graph.json \
          -DEXPECTED=mono.tools/architecture/expected_graph.json \
          -P mono.tools/architecture/CheckTargetGraph.cmake

# Regenerate the scripting manifest and the type declarations.
#
# The class table is the source; these are its output. Run this after changing a
# property declaration and review the diff — a change to what the manifest says
# is a change to what every script in every language can name.
bindings: (build "bindings")
    ./{{build}}/tools/bindings

# The manifest and the declarations against what the class table actually says.
#
# The same shape as `test-architecture`, and mandatory for the same reason: rule
# 6 says a rule the build does not check is documentation. Without this the
# manifest is a generated artefact nothing consumes, which is the failure this
# repository has already watched twice — `just docs-check` at v0.2 and
# `just preset=ci check` at v0.4, both of which stopped being true while still
# claiming to pass.
bindings-check: (build "bindings")
    ./{{build}}/tools/bindings --check

# Everything CI runs, in the order CI runs it, against one preset.
#
# Here so that "it passes locally" and "it passes in CI" mean the same thing.
# `.github/workflows/ci.yml` splits this across jobs by what each one needs
# installed — the point of the split is that the headless half runs on a machine
# with no graphics stack at all — but the checks are these and in this order:
# cheapest and most likely to fail first, so a misformatted file does not wait
# behind a compile.
#
# Not `preset=ci` by default, because that makes every warning fatal and the
# recipe is meant to be runnable mid-change. Use `just preset=ci check` for what
# the pipeline actually enforces.
check: format-check build test-all test-architecture bindings-check determinism replay-check
    @echo "check ok — format, build, tests, architecture, bindings, determinism, replay"

# Run the client. `just run --stats` passes flags straight through.
run *args: (build "client")
    ./{{build}}/client/client {{args}}

# Run the ECS demo scene with both debug panels open.
demo: (build "client")
    ./{{build}}/client/client --stats --graph

# Run the editor. `just edit --game My.agame` passes flags straight through.
#
# The only program in the repository where `RunService:IsStudio()` is true, and
# the one that writes a `.agame`. `just host --game X.agame` and
# `just run --game X.agame` read the same file back — a dedicated server and a
# single-player client respectively, which is what makes the format a module
# rather than something the editor owns.
edit *args: (build "studio")
    ./{{build}}/studio/studio {{args}}

# Run the headless server. `just host --ticks 100` passes flags through.
host *args: (build "server")
    ./{{build}}/server/server {{args}}

# Run the content origin. `just serve --root ./content` passes flags through.
serve *args: (build "cdn")
    ./{{build}}/cdn/cdn {{args}}

# A server and a client in one process, with no network between them.
#
# The diagnostic for "the replicated world is empty". `--connect` puts a
# handshake, a socket, framing, encryption and a bandwidth budget between the
# thing that serialises and the thing that draws, and a blank scene is equally
# consistent with any of them. This cuts all of it and prints a column per
# stage, so the first column that stops making sense is the answer.
# mono.unified_server_client/AGENTS.md says how to read it.
unified *args: (build "unified_server_client")
    ./{{build}}/unified_server_client/unified_server_client {{args}}

# Two runs of one scene, compared byte for byte.
#
# The determinism guarantee, checked rather than claimed. A recording is one
# snapshot plus every envelope applied since, and both halves are written in a
# stable order — so two runs of the same scene produce identical files, and any
# difference is a wall clock, a pointer address, or an unordered container that
# reached the simulation.
#
# Same binary, same machine. Cross-machine agreement is deliberately not
# promised: floating point differs between compilers and chips.
determinism entities="512" ticks="200": (build "server")
    @rm -f .cache/determinism-a.rec .cache/determinism-b.rec
    ./{{build}}/server/server --entities {{entities}} --ticks {{ticks}} --unpaced         --record .cache/determinism-a.rec > /dev/null
    ./{{build}}/server/server --entities {{entities}} --ticks {{ticks}} --unpaced         --record .cache/determinism-b.rec > /dev/null
    @cmp .cache/determinism-a.rec .cache/determinism-b.rec         || (echo "FAIL: two runs of the same scene diverged" && exit 1)
    @echo "determinism ok — {{ticks}} ticks over {{entities}} entities, byte-identical"
    @rm -f .cache/determinism-a.rec .cache/determinism-b.rec

# A recorded run, replayed, and the replay recorded again.
#
# Stronger than the above: it proves the *replay* path reproduces the run rather
# than merely that two live runs agree. A snapshot carries state and never code,
# so the replaying process registers the same systems — which it does by being
# the same program.
replay-check entities="256" ticks="120": (build "server")
    @rm -f .cache/replay-source.rec .cache/replay-again.rec
    ./{{build}}/server/server --entities {{entities}} --ticks {{ticks}} --unpaced         --record .cache/replay-source.rec > /dev/null
    ./{{build}}/server/server --replay .cache/replay-source.rec         --record .cache/replay-again.rec > /dev/null
    @test -f .cache/replay-again.rec         || (echo "FAIL: replaying with --record wrote nothing" && exit 1)
    @cmp .cache/replay-source.rec .cache/replay-again.rec         || (echo "FAIL: the replay did not reproduce the run it replayed" && exit 1)
    @echo "replay ok — {{ticks}} barriers reproduced, byte-identical"
    @rm -f .cache/replay-source.rec .cache/replay-again.rec

# Configure and build with no client at all, which is how the tier split is
# proved rather than asserted: the staged server/ gets no shaders/ directory.
check-server-is-headless:
    cmake --preset server > /dev/null
    cmake --build --preset server
    @test ! -d .cache/build/server/server/shaders \
        || (echo "FAIL: the server staged a shaders/ directory" && exit 1)
    @test ! -d .cache/build/server/client \
        || (echo "FAIL: the client was built into a server-only preset" && exit 1)
    @echo "server contains no graphics stack"

# The origin on its own, which is how repo_layout.md §11's claim is proved
# rather than asserted: it configures and builds where there is no Vulkan SDK,
# no SDL and no shader compiler.
#
# MONO_VENDORED_GLSLC is left alone deliberately. The preset builds no client,
# so the root CMakeLists never resolves a glslc at all — and if that stops being
# true, this recipe is where it shows up.
check-cdn-is-bare:
    cmake --preset cdn > /dev/null
    cmake --build --preset cdn
    @test ! -d .cache/build/cdn/cdn/shaders \
        || (echo "FAIL: the cdn staged a shaders/ directory" && exit 1)
    @test ! -d .cache/build/cdn/client \
        || (echo "FAIL: the client was built into a cdn-only preset" && exit 1)
    @test ! -d .cache/build/cdn/server \
        || (echo "FAIL: the server was built into a cdn-only preset" && exit 1)
    @echo "cdn contains no graphics stack and nothing else's program"

# The API reference, from the comments already in the headers.
#
# mono.tools/docgen rewrites plain `//` into the `///` Doxygen reads, so nothing
# in the sources has to carry a marker. Needs doxygen on PATH; the target says
# so and how to get it if it is missing.
docs: (build "docgen")
    cmake --build --preset {{preset}} --target docs

# The generated site, on a local port. Python is not a prerequisite of this
# repository — it is only ever a convenience here, and the recipe says so rather
# than failing with a traceback.
docs-serve port="8000": docs
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v python3 > /dev/null; then
        echo "python3 not found. Open {{build}}/docs/html/index.html directly."
        exit 1
    fi
    echo "http://localhost:{{port}}/"
    python3 -m http.server {{port}} --directory {{build}}/docs/html

# Fails if a public entity is undocumented, or if a comment is malformed.
#
# AGENTS.md rule 6: a rule the build does not check is documentation. "Public
# headers are documented" is a rule, so this is the half that checks it.
#
# Two Doxygen passes, because one cannot do both jobs: `EXTRACT_ALL` puts the
# whole public surface on the site and, in doing so, silently switches off the
# warning about the parts of it nobody documented.
docs-check: (build "docgen") docs
    #!/usr/bin/env bash
    set -euo pipefail
    log={{build}}/docs/warnings.txt
    if [ -s "$log" ]; then
        echo "$(wc -l < "$log") malformed comment(s) or dangling link(s):"
        cat "$log"
        exit 1
    fi
    cmake --build --preset {{preset}} --target docs-check

# Every first-party .cpp and .hpp. The directory list is explicit rather than
# `find .` so that mono.vendor/ is never touched — reformatting a submodule
# turns every future update into a conflict.
mono_sources := "mono.engine mono.client mono.server mono.unified_server_client mono.cdn mono.tools mono.build"

# Finding it is two problems, not one.
#
# `.clang-format` sets `BinPackParameters: OnePerLine`, which is an enum
# introduced in clang-format 21. Every older version reads that key as a boolean
# and stops with "invalid boolean" — a message that reads like the config is
# broken rather than the tool being too old, and which sent one person down the
# wrong path already.
#
# And installing clang-format-21 on Ubuntu leaves no unversioned `clang-format`
# on PATH, so looking for that name alone finds nothing on a machine that has
# exactly the right tool sitting next to it.
#
# So: search the versioned names too, and check the version rather than trusting
# whatever answers to the bare name.
find-clang-format := '''
    cf=""
    for candidate in clang-format clang-format-23 clang-format-22 clang-format-21; do
        command -v "$candidate" > /dev/null || continue
        major=$("$candidate" --version | sed -nE 's/.*version ([0-9]+)\..*/\1/p')
        if [ -n "$major" ] && [ "$major" -ge 21 ]; then cf="$candidate"; break; fi
    done
    if [ -z "$cf" ]; then
        echo "no clang-format 21 or newer on PATH (.clang-format needs 21)." >&2
        echo "  install:  sudo apt install clang-format-21     # apt.llvm.org" >&2
        echo "  link it:  sudo update-alternatives --install /usr/bin/clang-format \\" >&2
        echo "                clang-format /usr/bin/clang-format-21 100" >&2
        exit 1
    fi
'''

format:
    #!/usr/bin/env bash
    set -euo pipefail
    {{find-clang-format}}
    # Parenthesised: without it the -o binds loosely and the file set depends
    # on which find you have.
    find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
        | xargs -0 "$cf" -i
    echo "formatted $(find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) | wc -l) file(s) with $cf"

# Fails if anything is misformatted, and changes nothing. What CI wants.
#
# Missing tool is a failure, not a skip. A check that exits 0 when it did not
# run is worse than no check at all: CI goes green having verified nothing, and
# everyone downstream reads that green as "formatting is fine".
format-check:
    #!/usr/bin/env bash
    set -euo pipefail
    {{find-clang-format}}
    find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
        | xargs -0 "$cf" --dry-run --Werror

clean:
    rm -rf .cache/build

# Everything derived, including the test cache.
clean-all:
    rm -rf .cache
