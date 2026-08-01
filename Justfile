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

# Only the suites a change could have affected, by cascading signature hash.
test: build
    ./{{build}}/tools/testrunner --build {{build}}

# Every test, whatever changed.
test-all: build
    ./{{build}}/tools/testrunner --build {{build}} --all

# What the runner would run, and why, without running anything.
test-list: build
    ./{{build}}/tools/testrunner --build {{build}} --list

# The architecture test on its own — the target graph against the expectation.
# Needs a configure, not a build: it reads what CMake emitted.
test-architecture:
    cmake --preset {{preset}} > /dev/null
    cmake -DGRAPH={{build}}/target-graph.json \
          -DEXPECTED=mono.tools/architecture/expected_graph.json \
          -P mono.tools/architecture/CheckTargetGraph.cmake

# Run the client. `just run --stats` passes flags straight through.
run *args: (build "client")
    ./{{build}}/client/client {{args}}

# Run the ECS demo scene with both debug panels open.
demo: (build "client")
    ./{{build}}/client/client --stats --graph

# Run the headless server. `just host --ticks 100` passes flags through.
host *args: (build "server")
    ./{{build}}/server/server {{args}}

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
mono_sources := "mono.engine mono.client mono.server mono.tools mono.build"

format:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v clang-format > /dev/null; then
        echo "clang-format not found. Install it, or skip formatting and say so"
        echo "in the pull request — do not hand-format to match."
        exit 1
    fi
    # Parenthesised: without it the -o binds loosely and the file set depends
    # on which find you have.
    find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
        | xargs -0 clang-format -i
    echo "formatted $(find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) | wc -l) file(s)"

# Fails if anything is misformatted, and changes nothing. What CI wants.
format-check:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v clang-format > /dev/null; then
        echo "clang-format not found; skipping the format check"
        exit 0
    fi
    find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
        | xargs -0 clang-format --dry-run --Werror

clean:
    rm -rf .cache/build

# Everything derived, including the test cache.
clean-all:
    rm -rf .cache
