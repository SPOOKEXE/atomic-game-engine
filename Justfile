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
# server preset and then run the *dev* preset's binaries - a mismatch nothing
# reported. One override, applied before anything is derived, is the only form
# that cannot go wrong that way.
preset := "dev"
build  := ".cache/build/" + preset

_default:
    @just --list --unsorted

# Submodules, and anything else a fresh clone needs before it can configure.
setup: install-hooks
    git submodule update --init --recursive --depth 1
    # shaderc pins glslang, SPIRV-Tools and SPIRV-Headers in its own DEPS file
    # rather than as submodules, so the line above clones shaderc and leaves
    # third_party/ empty. Without this the client configure stops in
    # MonoVendor.cmake pointing back here.
    python3 mono.vendor/shaderc/utils/git-sync-deps
    @echo "vendors ready"

# Point git at the hooks this repository carries. Once per clone.
#
# **`core.hooksPath`, so the hook is a file in the tree rather than a thing
# somebody remembers to copy.** `.git/hooks/` is not cloned and not reviewable;
# `.githooks/` is both. The cost is that the pointer is per-clone local config,
# which is why `just setup` runs this - a fresh clone's first command installs
# it, and nobody has to be told twice.
#
# Safe to re-run, and it refuses rather than silently replacing a hooksPath
# somebody else set - a hook you did not install running on your push is worse
# than no hook.
#
# Installs `.githooks/pre-push`, which builds `preset=ci` before a push.
install-hooks:
    #!/usr/bin/env bash
    set -euo pipefail
    existing=$(git config --local --get core.hooksPath || true)
    if [ -n "$existing" ] && [ "$existing" != ".githooks" ]; then
        echo "core.hooksPath is already '$existing', not .githooks - leaving it alone." >&2
        echo "  Set it yourself if you meant to:  git config core.hooksPath .githooks" >&2
        exit 1
    fi
    git config --local core.hooksPath .githooks
    chmod +x .githooks/*
    echo "hooks installed - .githooks/pre-push builds preset=ci before a push."
    echo "Escape one deliberately with: git push --no-verify"

# Configure one preset. Safe to re-run; CMake reuses the cache.
configure:
    cmake --preset {{preset}}

# Build everything, or one target. `just build engine_ecs` builds one module.
#
# **The configure runs when there is no build directory, and otherwise Ninja
# decides.** An unconditional `cmake --preset` here would re-configure on every
# build including the ones with nothing to do, which is the whole of what a null
# build appears to cost. It is also unnecessary: `build.ninja` carries a
# regeneration edge over every `CMakeLists.txt`, every `.cmake` module and the
# `CONFIGURE_DEPENDS` glob check, so `cmake --build` re-configures itself the
# moment any of those changes.
#
# `CMakePresets.json` is the one input that edge does not name, and a preset
# that gains a cache variable would otherwise reach nobody who did not know to
# re-configure by hand. Hence the timestamp test rather than a bare existence
# test.
build target="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -f {{build}}/build.ninja ] || [ CMakePresets.json -nt {{build}}/CMakeCache.txt ]; then
        cmake --preset {{preset}} > /dev/null
    fi
    cmake --build --preset {{preset}} {{ if target == "" { "" } else { "--target " + target } }}

# One program and only its dependencies.
client: (build "client")
server: (build "server")
cdn: (build "cdn")
studio: (build "studio")
launcher: (build "launcher")

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
# build, and the danger is not that it is slower - it is that the *ratios*
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

# The architecture test on its own - the target graph against the expectation.
# Needs a configure, not a build: it reads what CMake emitted.
#
# Then the fixtures, and they are the half worth explaining. This check walks an
# expectation and reports what it finds wrong; an expectation it fails to parse
# walks as zero entries and reports success, so the check is the one in the
# repository that can go green by doing nothing. Each directory under
# `mono.tools/architecture/tests/` is a graph and an expectation that must fail,
# with the message it must produce in its own `expect` file, plus one `clean`
# pair that must pass so that "everything fails" is not how the suite goes green.
test-architecture:
    #!/usr/bin/env bash
    set -euo pipefail
    cmake --preset {{preset}} > /dev/null
    cmake -DGRAPH={{build}}/target-graph.json \
          -DEXPECTED=mono.tools/architecture/expected_graph.json \
          -P mono.tools/architecture/CheckTargetGraph.cmake

    failures=0
    for fixture in mono.tools/architecture/tests/*/; do
        name=$(basename "$fixture")
        want=$(cat "$fixture/expect")
        set +e
        out=$(cmake -DGRAPH="$fixture/graph.json" -DEXPECTED="$fixture/expected.json" \
                    -P mono.tools/architecture/CheckTargetGraph.cmake 2>&1)
        code=$?
        set -e

        if [ -z "$want" ]; then
            if [ "$code" -ne 0 ]; then
                echo "fixture '$name' should pass and did not:"
                printf '%s\n' "$out" | sed 's/^/    /'
                failures=$((failures + 1))
            fi
        elif [ "$code" -eq 0 ]; then
            echo "fixture '$name' should fail and passed. The check has stopped catching it."
            failures=$((failures + 1))
        elif ! printf '%s' "$out" | grep -qF "$want"; then
            echo "fixture '$name' failed for the wrong reason. Wanted: $want"
            printf '%s\n' "$out" | sed 's/^/    /'
            failures=$((failures + 1))
        fi
    done

    if [ "$failures" -ne 0 ]; then
        echo "architecture fixtures: $failures wrong"
        exit 1
    fi
    count=$(find mono.tools/architecture/tests -mindepth 1 -maxdepth 1 -type d | wc -l)
    echo "-- architecture fixtures ok - $count fixture(s)"

# The four architecture rules that live in source text rather than in the graph.
#
# **The other half of `test-architecture`.** That one reads CMake's own output
# and checks the module set, the tiers, the link sets and the layer heights.
# These four are invisible there, and `docs/CODE_ARCH.md` §11 listed all four as
# convention until v0.19 - root `AGENTS.md` rule 2 (the ECS owns the storage),
# rule 3 (nothing crossing a world boundary is a pointer), rule 4 (a `Name` is
# serialised as its text) and §3's rule that a public header is one somebody
# outside the module includes.
#
# **The fixtures run before the repository does, and that ordering is the
# point.** `sourcecheck` reads C++ as text, so a tree it fails to read scans as
# zero declarations and reports success - the failure `mono.tools/architecture`
# already has a fixture directory for, with a wider mouth. Three of the four
# rules find nothing at all in this repository, so a green scan means nothing
# until the scanner has been shown to bite. Each fixture names the sentence it
# must produce, and the runner also holds the exit status to `Gating`: a rule
# that found something and did not fail the build is the same bug as a rule that
# found nothing.
#
# `public-header` reports and never gates. Root `AGENTS.md` warns that an
# unwired subsystem is not dead code - `IssueContentGrant` and the viewpoint
# pair are deliberate forward API - so this one prints a list somebody reads
# rather than a verdict, and `// arch-waiver public-header: <reason>` in the
# header takes one off the list.
source-check: (build "sourcecheck")
    #!/usr/bin/env bash
    set -euo pipefail
    failures=0
    for fixture in mono.tools/sourcecheck/tests/fixtures/*/; do
        [ -f "$fixture/expect" ] || continue
        name=$(basename "$fixture")
        want=$(cat "$fixture/expect")
        set +e
        out=$(./{{build}}/tools/sourcecheck "$fixture/tree" 2>&1)
        code=$?
        set -e

        # The open findings on the three rules that gate, which is what the exit
        # status has to agree with.
        gating=$(printf '%s\n' "$out" | awk '/^(ecs-copy|world-pointer|name-id) - / { total += $3 } END { print total + 0 }')

        if [ -z "$want" ]; then
            if [ "$code" -ne 0 ] || printf '%s\n' "$out" | grep -q '^  open '; then
                echo "fixture '$name' should be clean and is not:"
                printf '%s\n' "$out" | sed 's/^/    /'
                failures=$((failures + 1))
            fi
        elif ! printf '%s' "$out" | grep -qF "$want"; then
            echo "fixture '$name' did not report what it exists to report. Wanted: $want"
            printf '%s\n' "$out" | sed 's/^/    /'
            failures=$((failures + 1))
        elif [ "$gating" -gt 0 ] && [ "$code" -eq 0 ]; then
            echo "fixture '$name' found a gating violation and let the build pass."
            failures=$((failures + 1))
        elif [ "$gating" -eq 0 ] && [ "$code" -ne 0 ]; then
            echo "fixture '$name' failed the build for a rule that only reports."
            failures=$((failures + 1))
        fi
    done

    if [ "$failures" -ne 0 ]; then
        echo "sourcecheck fixtures: $failures wrong"
        exit 1
    fi
    # A glob that matched nothing would leave `failures` at zero and print a
    # tick, which is the exact failure this recipe exists to refuse.
    count=$(find mono.tools/sourcecheck/tests/fixtures -mindepth 1 -maxdepth 1 -type d | wc -l)
    if [ "$count" -lt 13 ]; then
        echo "only $count fixture(s) under mono.tools/sourcecheck/tests/fixtures - there are thirteen."
        exit 1
    fi
    echo "-- sourcecheck fixtures ok - $count fixture(s)"

    ./{{build}}/tools/sourcecheck .

# The module index for the API reference, from the graph and the AGENTS.md files.
#
# `mono.tools/docgen/pages/Modules.md` was hand-maintained and listed ten of the
# engine's twenty-nine modules, which is what a hand-maintained list of a
# generated fact always becomes. It is now a walk of the tree ordered by the
# layers, so a module that exists is on the page.
docs-pages:
    cmake -DEXPECTED=mono.tools/architecture/expected_graph.json \
          -DROOT="$(pwd)" \
          -DOUT=mono.tools/docgen/pages/Modules.md \
          -P mono.tools/architecture/WriteModulePages.cmake

# The checked-in page against the tree. Rule 6: adding a module without running
# `just docs-pages` is exactly how the old page fell nineteen modules behind.
docs-pages-check:
    cmake -DEXPECTED=mono.tools/architecture/expected_graph.json \
          -DROOT="$(pwd)" \
          -DOUT=mono.tools/docgen/pages/Modules.md \
          -DCHECK=YES \
          -P mono.tools/architecture/WriteModulePages.cmake

# Every first-party object against the header dependencies ninja recorded for it.
#
# **The check for a bug that produced a working build and a broken binary.** At
# v0.15 fifty-seven of four hundred first-party objects had `#deps 0` - no
# recorded headers at all - so a change to a header rebuilt some translation
# units and not others, and the result held two different layouts of one struct.
# `studio::Options` grew a field, `Settings.ControlPort` read as zero instead of
# minus one in the objects that had not been rebuilt, and the studio died in
# `control::Server::Start` on a pimpl that was not there.
#
# **A full `cmake --build` does not find it and no test can.** Ninja believed
# everything was current, so the build was silent and green; the suites link
# whatever the objects say and cannot see that two of them disagree. That is the
# third category `AGENTS.md` rule 6 refuses to allow - a constraint that lives in
# nobody's memory - which is why this is a recipe rather than a paragraph.
#
# The pattern was files added since the deps log was last rebuilt, so this is
# worth running after anything arrives through a `CONFIGURE_DEPENDS` glob.
#
# Fix what it reports by deleting the named objects and building again: a
# rebuilt object records its headers, and only a missing record is the fault.
deps-check: build
    #!/usr/bin/env bash
    set -euo pipefail
    cd {{build}}
    missing=0
    total=0
    while read -r object; do
        total=$((total + 1))
        recorded=$(ninja -t deps "$object" 2>/dev/null | head -1 | grep -oE '#deps [0-9]+' | grep -oE '[0-9]+' || true)
        if [ "${recorded:-0}" = "0" ]; then
            missing=$((missing + 1))
            echo "no recorded headers: $object"
        fi
    done < <(find . -name '*.cpp.o' -path '*mono.*')
    if [ "$missing" -gt 0 ]; then
        echo "deps-check FAILED - $missing of $total first-party object(s) track no headers."
        echo "A header change will not reach them, and the binary will mix struct layouts."
        exit 1
    fi
    echo "deps ok - $total first-party object(s) track their headers"

# Build outputs whose source is gone.
#
# **A stale artefact is not untidiness, it is a wrong answer.** Four separate
# times in one session this repository answered a question with a file that
# should not have existed. An orphaned `test_nodeview` binary, left when that
# module was reverted, was still discovered by the runner and inflated the suite
# count by three. Thirty-five `.spv` files, left when the built-in shaders moved
# from `render` to `resources`, made `shader-check` report 49 modules where
# there are 14. An object for a source `bake` no longer has turned `deps-check`
# red. In every case the artefact was not merely present, it was *consulted*.
#
# Ninja already knows which outputs its current graph produces, so `-t cleandead`
# is the whole check rather than a heuristic over file names. It is why this
# recipe is six lines and not a scanner that has to be kept in step with the
# layout.
#
# This reports and never deletes, so it is safe inside `check`. `orphan-clean`
# is the fix, and it sweeps every configured preset rather than this one,
# because a stale `release` or `bench` tree answers just as loudly and is
# consulted far less often.
orphan-check: build
    #!/usr/bin/env bash
    set -euo pipefail
    listing=$(ninja -C {{build}} -n -t cleandead 2>&1 || true)
    # ninja says "N files." when asked and "Cleaning... N files." when doing, so
    # take the count off whichever it printed rather than anchoring on one.
    dead=$(printf '%s\n' "$listing" | grep -oE '[0-9]+ files' | tail -1 | grep -oE '[0-9]+' || true)
    dead=${dead:-0}
    if [ "$dead" -gt 0 ]; then
        printf '%s\n' "$listing" | grep '^Remove ' | sed 's/^Remove /  /' | head -20 || true
        if [ "$dead" -gt 20 ]; then echo "  ... and $((dead - 20)) more"; fi
        echo "orphan-check FAILED - $dead output(s) in {{build}} are no longer produced by the build."
        echo "A file whose source is gone still gets linked, counted and scanned."
        echo "Run 'just orphan-clean' to sweep every configured preset."
        exit 1
    fi
    echo "orphans ok - every output in {{build}} is still produced by the build"

# Delete build outputs the graph no longer produces, in every preset.
#
# Safe by construction: ninja removes only what its own graph says is dead, so
# this can never take a file the next build would have wanted. It is a separate
# recipe rather than something `build` does quietly, because a build that
# deletes files nobody asked it to delete is a build people stop trusting.
orphan-clean:
    #!/usr/bin/env bash
    set -euo pipefail
    total=0
    for dir in .cache/build/*/; do
        [ -f "$dir/build.ninja" ] || continue
        removed=$(ninja -C "$dir" -t cleandead 2>&1 | grep -oE '[0-9]+ files' | tail -1 | grep -oE '[0-9]+' || true)
        removed=${removed:-0}
        total=$((total + removed))
        printf '  %-12s %s file(s)\n' "$(basename "$dir")" "$removed"
    done
    echo "orphan-clean ok - $total dead output(s) removed"

# Every compiled shader against the resource contract SDL documents.
#
# **The half of `docs/DEFERRED.md` D00001 that does not need a Mac.** That entry
# has said since v0.1 that macOS compiles SPIR-V and not MSL, with no trigger on
# it because nobody here has the machine that would trip it. Most of what that
# machine would find is not about Metal: `SDL_CreateGPUShader` documents one
# resource layout per shader format, and every one of them - MSL, DXIL, DXBC - is
# derived from the descriptor sets and bindings in the SPIR-V. If those are
# wrong, the translation is wrong, and the SPIR-V is here.
#
# So this checks what is checkable from Linux: one entry point per module, named
# what the renderer asks for, at the stage the filename claims; every resource
# explicitly decorated; every resource in the descriptor set SDL's contract names
# for its stage; bindings contiguous within a set, because every other format
# numbers them by counting and a gap shifts everything after it; and no SPIR-V
# capability outside an allowlist of what MSL can express - `double` being the
# one that compiles happily for Vulkan and cannot be translated at all.
#
# **Since v0.15 it also reads the translation back.** The build writes an `.msl`
# beside every `.spv`, and this holds each one to the module it came from: one
# entry point, named `main0` because MSL reserves `main`, qualified for the right
# stage, and every `[[texture]]`, `[[buffer]]` and `[[sampler]]` index the one
# SDL's documented order derives. That last check is not decoration - SPIRV-Cross
# left to itself numbered `opaque.frag`'s textures in id order and put the last
# one in the descriptor set at `[[texture(0)]]`.
#
# **It still proves nothing about a Metal device.** There is no Metal compiler
# here, so "valid MSL" means balanced, prefaced and bound where SDL will look,
# and no more. A check that asserted something this machine cannot observe would
# be the same untested claim D00001 already records, arriving a second time.
#
# `--quiet` here because `just check` runs it. Drop it - `shadercheck
# .cache/build/dev/shaderstage` - for the per-shader table, including the
# `[[texture(n)]]` and `[[buffer(n)]]` index each resource lands on.
shader-check: build
    #!/usr/bin/env bash
    set -euo pipefail
    stage="{{build}}/shaderstage"
    if [ ! -d "$stage" ]; then
        # A server or cdn preset configures no shader compiler and stages no
        # shaders. Said out loud, for `just typecheck`'s reason: a check that
        # passes by having nothing to do is one nobody notices has stopped
        # running.
        echo "shader-check skipped: preset {{preset}} builds no client, so no shader was compiled."
        exit 0
    fi
    ./{{build}}/tools/shadercheck --quiet "$stage"

# Regenerate the ECS component catalogue.
#
# **Generated because a hand-written list of a hundred and twenty-nine
# components is wrong within a month, and wrong quietly.** Everything mechanical
# about a component - its registered name, its size, whether it is a tag,
# whether it can be saved, whether it has a compact wire form, whether it has
# padding a raw writer would leak into a file - is already in `ecs::Components`.
# The tool walks that table.
#
# The one thing the table cannot know is what a component is *for*, so that
# lives in `mono.tools/componentdoc/purposes.md`, one line per component, and
# **that is the file to edit.** `docs/ECS_COMPONENTS.md` is output.
#
# The tool builds in a `server` preset as well as a `dev` one: every module it
# links is `shared`, which is correct rather than incidental - a dedicated
# server holds the same components a client does.
components: (build "componentdoc")
    ./{{build}}/tools/componentdoc

# The catalogue against the registry, and the registry against the purposes.
#
# Three ways to fail, and the third is the one worth having: the checked-in
# catalogue is stale; a purpose line names a component nothing registers, which
# is what a rename leaves behind; or a registered component has no purpose line,
# which is what adding one leaves behind. Without the third, adding a component
# would regenerate a row reading "undocumented" and nothing would object.
components-check: (build "componentdoc")
    ./{{build}}/tools/componentdoc --check

# Regenerate the scripting manifest and the type declarations.
#
# The class table is the source; these are its output. Run this after changing a
# property declaration and review the diff - a change to what the manifest says
# is a change to what every script in every language can name.
bindings: (build "bindings")
    ./{{build}}/tools/bindings

# The manifest and the declarations against what the class table actually says.
#
# The same shape as `test-architecture`, and mandatory for the same reason: rule
# 6 says a rule the build does not check is documentation. Without this the
# manifest is a generated artefact nothing consumes, which is the failure this
# repository has already watched twice - `just docs-check` at v0.2 and
# `just preset=ci check` at v0.4, both of which stopped being true while still
# claiming to pass.
bindings-check: (build "bindings")
    ./{{build}}/tools/bindings --check

# Every authored script against the declarations, in both languages.
#
# **The other half of `bindings-check`.** That one asks whether the declarations
# still match the class table; this asks whether the scripts still match the
# declarations. A property removed from the class table regenerates cleanly and
# leaves every script that named it broken, with nothing reporting it until the
# scene fails to build.
#
# The Luau half is `mono.tools/scriptcheck`, which exists because upstream's
# `luau-analyze` has no way to load a definition file - see MonoVendor.cmake.
# The TypeScript half is `tsc` against the checked-in `tsconfig.json`, which
# already lists the generated `.d.ts` as its type root.
#
# **The compiler is the pinned one, not whatever `bunx tsc` resolves to.**
# `package.json` names an exact version and this runs `node_modules/.bin/tsc`,
# so an upgrade is a commit rather than a morning. Installing is idempotent and
# offline once the package is cached.
#
# **`tsc` is skipped rather than fatal when no runner is installed.** It is the
# one check here that needs something outside the C++ toolchain, and a
# prerequisite list that grows a Node runtime for two example files is a worse
# trade than a check that says out loud when it did not run.
typecheck: (build "scriptcheck")
    #!/usr/bin/env bash
    set -euo pipefail
    ./{{build}}/tools/scriptcheck mono.engine/examples/*.luau

    if command -v bun > /dev/null; then
        bun install --silent
    elif command -v npm > /dev/null; then
        npm install --silent --no-audit --no-fund
    else
        echo "typecheck ok - luau. TypeScript skipped: no bun or npm on PATH."
        exit 0
    fi

    ./node_modules/.bin/tsc --noEmit
    echo "typecheck ok - luau and typescript $(./node_modules/.bin/tsc --version | cut -d' ' -f2)"

# The same scripts again, through the language server an editor actually runs.
#
# **`just typecheck` and an editor are two frontends over one definitions file,
# and they have disagreed.** `scriptcheck` registers `importedTypeBindings["Enum"]`
# on its own frontend, which is a host-side call no definitions file can make - so
# `local face: Enum.NormalId` compiled, passed, and was underlined in the editor
# for three versions. `docs/retired/DEFERRED.md` D00031 is that gap and the patch
# that closed it; this recipe is what stops it reopening unnoticed, because a
# patch that stops applying and a spelling that stops resolving both land here.
#
# It also enables Luau's feature flags, which `scriptcheck` does not - that is how
# the `declare class` deprecation was caught.
#
# **1.5 s over every example**, so it is in `just check` rather than beside it.
# What is not free is the first run: `luau-lsp` compiles its own copy of Luau -
# 11 minutes of CPU, 39 s wall on 24 cores, once. Afterwards the dependency is a
# no-op.
typecheck-editor: luau-lsp
    ./.cache/build/luau-lsp/luau-lsp analyze --settings=luau-lsp.json mono.engine/examples/*.luau
    @echo "typecheck-editor ok - every example agrees with the language server"

# The editor, with its control surface open for a Model Context Protocol client.
#
# **Two processes and one port.** This starts the editor listening on loopback;
# `mono.tools/mcpbridge` is what an MCP client launches, and it pumps bytes
# between the client's stdio and this port. `mono.studio/src/Control.cpp` carries
# why the editor listens rather than being launched.
#
# Off unless asked for: the surface runs scripts, writes properties and saves
# files for whatever connects, so opening it is a decision rather than a default.
# `just mcp` opens 8738 - the port `.mcp.json` and RUNNING.md name for the
# editor, and they have to agree or a client connects to nothing; `just mcp 9001 --game My.agame` picks a port and passes
# the rest through to the editor.
#
# The editor, listening for a Model Context Protocol client.
mcp port="8738" +args="--width 1600": (build "studio") (build "mcpbridge")
    #!/usr/bin/env bash
    set -euo pipefail
    echo "editor: 127.0.0.1:{{port}}"
    echo "client: $(pwd)/{{build}}/tools/mcpbridge --port {{port}}"
    echo ""
    ./{{build}}/studio/studio --mcp-port {{port}} {{args}}

# The editor's language server, built from `mono.vendor/luau-lsp`.
#
# **The one vendor this build never compiles**, which is why it is a recipe of
# its own rather than a target. `just setup` walks past the submodule -
# `update = none` in `.gitmodules`, and the reason is written there - so this
# clones it on first run. Nothing else in the repository needs it: `just
# typecheck` gates on `mono.tools/scriptcheck`, which links our Luau.
#
# It is built in a tree of its own because it brings its own Luau and declares
# the same `Luau.*` target names ours does. Adding it to this build fails at
# configure time; `.gitmodules` carries the full argument.
#
# **`-Wno-error=maybe-uninitialized`, and the narrowness is the point.** luau-lsp
# hardcodes `-Wall -Werror` with no option to disable it, and GCC 13 reports a
# false positive in `src/operations/CallHierarchy.cpp`: a `std::string` inside an
# `std::optional` inside the `FunctionName` pair, reported against
# `bits/basic_string.h` rather than against any line upstream wrote. So the build
# fails on a warning about vendored code in a vendored tree.
# `MonoVendor.cmake` turns Luau's own `LUAU_WERROR` off for exactly this reason.
#
# A blanket `-Wno-error` does not work here: `CMAKE_CXX_FLAGS` lands *before*
# their `target_compile_options`, so the later `-Werror` wins. A specific
# `-Wno-error=<warning>` takes precedence over the blanket form whatever the
# order, which is why this names the warning rather than silencing all of them -
# every other warning upstream cares about still fails the build.
#
# **`-include cstdint` was the second one and is gone.** It was here because
# luau-lsp pinned a Luau from before GCC 13 stopped including `<cstdint>`
# transitively, so `Ast/src/StringUtils.cpp` named `uint8_t` without including
# it. The v0.15 bump to the revision luau-lsp `d5df9af` pins carries the fix, and
# the flag was removed after building without it rather than on the assumption
# that a newer tree would be fine. `docs/DEFERRED.md` D00019 is the bump.
#
# Flags rather than patches wherever a flag will do. Where one will not, the
# patch is a file under `mono.vendor/patches/luau-lsp/` - the third shape
# `mono.vendor/AGENTS.md` argues for, taken over a fork so that nothing has to be
# pushed to a remote we do not own.
#
# **The patch is applied to a copy, never to the submodule.** Applying it in
# place left `mono.vendor/luau-lsp` modified in every `git status` from then on,
# which is noise at best and a submodule pointer committed by accident at worst.
# `scripts/vendor-tree.sh` archives the pinned commit into `.cache/vendor/` and
# patches it there; the contract is that a directory under
# `mono.vendor/patches/<name>/` means "this vendor builds from a copy", and the
# script asserts the submodule is pristine on the way through.
#
# The editor's language server, built from a patched copy of `mono.vendor/luau-lsp`.
luau-lsp:
    #!/usr/bin/env bash
    set -euo pipefail
    src=$(scripts/vendor-tree.sh luau-lsp)

    # **The two Luaus must be one Luau.** The editor type-checks with the copy
    # luau-lsp brings and `just typecheck` with `mono.vendor/luau`; if they drift
    # apart, an author gets diagnostics from a language the engine does not run -
    # which is worse than no editor support, because it looks authoritative.
    # Checked rather than written down, because rule 6 says a rule the build does
    # not check is documentation.
    ours=$(git -C mono.vendor/luau rev-parse HEAD)
    theirs=$(git -C mono.vendor/luau-lsp/luau rev-parse HEAD)
    if [ "$ours" != "$theirs" ]; then
        echo "the two vendored Luaus disagree:" >&2
        echo "  mono.vendor/luau          $ours" >&2
        echo "  mono.vendor/luau-lsp/luau $theirs" >&2
        echo "" >&2
        echo "Pin them to one commit. .gitmodules says which and why; the ceiling" >&2
        echo "is whatever luau-lsp compiles against. docs/DEFERRED.md D00019." >&2
        exit 1
    fi

    # **A build tree remembers where its source was**, and CMake stops rather
    # than adapting when that moves - so the first run after the patched tree
    # became a copy has to start over. It costs the eleven minutes of CPU once
    # and then never again; without it the run fails on a CMakeCache mismatch
    # that reads like a broken checkout.
    if [ -f .cache/build/luau-lsp/CMakeCache.txt ] &&
       ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$(pwd)/$src" .cache/build/luau-lsp/CMakeCache.txt; then
        echo "luau-lsp: source is now $src, reconfiguring from scratch"
        rm -rf .cache/build/luau-lsp
    fi

    cmake -S "$src" -B .cache/build/luau-lsp -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="-Wno-error=maybe-uninitialized" > /dev/null
    cmake --build .cache/build/luau-lsp --target luau-lsp
    echo ""
    echo "luau-lsp built: $(pwd)/.cache/build/luau-lsp/luau-lsp"
    echo "Point your editor at it - see RUNNING.md, 'Autocomplete while you write one'."

# Every check there is, in the order to run them, against one preset.
#
# **The whole guarantee, and it is local.** No machine other than this one runs
# any of it: there is no workflow on GitHub and there will not be one - the
# repository's owner decided that, and `docs/retired/DEFERRED.md` D00005 carries
# the decision and what it costs. So this recipe is not "what CI runs". It is
# what a person runs before a push.
#
# The order is cheapest and most likely to fail first, so a misformatted file
# does not wait behind a compile.
#
# Not `preset=ci` by default, because that makes every warning fatal and the
# recipe is meant to be runnable mid-change. Use `just preset=ci check` for the
# strictest configuration this repository has.
#
# **The one part of it that is not manual is the `ci` build**, which
# `.githooks/pre-push` does for you - because that is the half that has gone
# uncompilable three times while being described as the standard, twice inside
# v0.15 alone, and a check nobody runs stops being true.
check: format-check em-dash-check build test-all test-architecture source-check docs-pages-check shader-check check-one-node-graph bindings-check components-check typecheck typecheck-editor determinism replay-check client-smoke orphan-check
    @echo "check ok - format, em dashes, build, tests, architecture, source rules, shaders, bindings, typecheck, editor, determinism, replay, orphans"

# Run the launcher - the window that starts any of the others.
#
# **Builds only itself, and that is worth knowing before the first run.** This
# launcher links none of the programs it starts; it finds them staged beside it
# and asks each one what it accepts. So a `just launch` in a tree where nothing
# else has been built opens a window with every mode greyed out and the reason
# on each row. `just build` first, or build the one program the mode needs.
#
# `just launch --mode cdn` opens straight onto a mode.
launch *args: (build "launcher")
    ./{{build}}/launcher/launcher {{args}}

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
# `just run --game X.agame` read the same file back - a dedicated server and a
# single-player client respectively, which is what makes the format a module
# rather than something the editor owns.
edit *args: (build "studio")
    ./{{build}}/studio/studio {{args}}

# The editor, driven with no display at all.
#
# **What makes the studio checkable by something that is not a person.** It
# loads a game, starts it, renders the world into an offscreen target and writes
# the result - no window, no compositor, no machine that has to be left alone.
# A scripted control or an agent reads the image instead of a screen.
#
# Not part of `just check`: it needs a GPU, and a build container that has none
# would fail a check about the editor for a reason that is not about the editor.
# **And a second shot, of the scene built to show content.** Every headless check
# this editor could run was a count, and a count says the same thing whether the
# models are right, squashed, inside-out, untextured or culled away - all five of
# which have happened. The mesh grid is the one scene whose whole job is to make
# content visible, so photographing it turns "13 placed, 7 meshes, 16 textures"
# into something somebody can actually disagree with.
#
# **Longer than twelve frames, because content arrives over several.** Naming a
# mesh is what fetches it, the fetch spans frames, and the grid grows on
# `Heartbeat` as bundles land - so a capture at frame ten is a picture of an empty
# plate and says nothing.
studio-smoke game="" out=".cache/studio-smoke.bmp" meshes=".cache/studio-meshes.bmp": (build "studio")
    @rm -f {{out}} {{meshes}}
    ./{{build}}/studio/studio --headless --frames 60 --run play         {{ if game == "" { "" } else { "--game " + game } }}         --capture {{out}} --width 960 --height 540
    @test -s {{out}} || (echo "FAIL: the headless editor wrote no capture" && exit 1)
    ./{{build}}/studio/studio --headless --frames 700 --run play         --capture-world MeshGrid --capture {{meshes}} --width 1280 --height 900
    @test -s {{meshes}} || (echo "FAIL: the headless editor wrote no mesh capture" && exit 1)
    @echo "studio ok - loaded, played and rendered with no display, into {{out}} and {{meshes}}"

# Press a button in a shipped client, with no display, and read the answer back.
#
# **The check `docs/DEFERRED.md` D00125 asked for, and the bug it names is why.**
# At v0.15 the shipped client did not route interface input at all - the router
# was constructed, read and never `Update`d, so a `TextButton` in a game never
# lit and its `Activated` never fired, while the same tree worked in the editor
# because the studio drives a router of its own. A button that does nothing in
# the shipped build and works in the editor is the worst version of that bug,
# because it is invisible to whoever is authoring.
#
# Closing it needed two things the client did not have: `--headless`, so the run
# needs no display, and `--click NAME`, so something can press. The press is
# synthesised into `input::Translator` as an ordinary SDL event and travels the
# path a real click travels - same translator, same `scene::InputState`, same
# `gui::Router`, same `Runtime::DeliverGuiEvents`. A click that took a shortcut
# past any of those would be a check of the shortcut.
#
# **It found a second one on its first run.** `examples::LoadScene` kept the only
# reference to the VM it created, so `RuntimeOf` answered null for a `--script`
# world and every gui event the router produced was delivered nowhere. The
# router was right, the events were right, and the last hop was missing.
#
# Not part of `just check`: it needs a GPU, for `studio-smoke`'s reason.
# **In `just check` since v0.19, and the reason is the component table.** The
# programs seal it after start-up, so a component registered during a tick now
# aborts rather than quietly taking an id that depends on which world got there
# first. `just determinism` and `just replay-check` already run the *server*, so
# that half was covered; nothing in the umbrella check ran the *client*, and the
# client is where five of the six late registrations found at v0.19 lived.
#
# A suite cannot replace this. `Store` construction and `RegisterSceneComponents`
# happen in a test too, but a lazily-registered resource only appears when the
# pass that uses it runs against a real scene - which is what this recipe does
# and what a unit test deliberately does not.
client-smoke: (build "client")
    #!/usr/bin/env bash
    set -euo pipefail
    scene="{{build}}/assets/examples/Interface.luau"
    test -f "$scene" || { echo "FAIL: no staged scene at $scene"; exit 1; }
    log=$(mktemp)
    trap 'rm -f "$log"' EXIT
    # **300 frames, and the margin is the point.** The press lands on frame 0 and
    # the release two frames later, but the script's handler does not run in the
    # same breath - at 60 frames the run ended before it had, about one time in
    # four, which is a flake that would read as the click being broken. The run
    # costs about a third of a second either way.
    # **Bounded, because this recipe cannot report a hang otherwise.** The run
    # takes about a third of a second and the client's teardown is where it has
    # gone wrong before - see `just client-exit`. Without this, a client that
    # never exits stops the build rather than failing it.
    timeout 120 ./{{build}}/client/client --headless --frames 300 --width 960 --height 540 \
        --script "$scene" --click Swatch3 > "$log" 2>&1 \
        || { echo "FAIL: the client exited $? (124 means it never exited at all)"; tail -20 "$log"; exit 1; }
    grep -q "click: pressed 'Swatch3'" "$log" \
        || { echo "FAIL: the client never found or pressed the button"; tail -20 "$log"; exit 1; }
    grep -q "interface: swatch 3 activated" "$log" \
        || { echo "FAIL: the button was pressed and its Activated never reached the script"; tail -20 "$log"; exit 1; }
    echo "client ok - pressed a button with no display and the script heard it"

# Run the client to a frame budget and check the process actually ends.
#
# **A hang is the one failure a `TEST_CASE` cannot report**, which is why this
# is a script with a `timeout` rather than a case in the suite: Catch2 has no
# way to fail something that never returns, so a hung teardown stops the run
# instead of failing it.
#
# The bug it was written for shipped at v0.15. `Client::Shutdown` released the
# GPU device while `InterfacePass` still held a raw pointer to it, so
# `~InterfacePass` released its glyph atlas through a freed device and blocked
# forever on a mutex that had been freed with it. `Run()` had already drawn its
# frames, printed its statistics and returned 0 - a hung run's log and a
# healthy one's are identical up to the last line.
#
# **And it was invisible because teardown had two paths.** `Renderer::Shutdown`
# was guarded by `if (Window)`, so a headless run skipped the device entirely
# and every check this repository had walked an order the shipped windowed
# client never takes. Both are one path now, and this checks both.
#
# Not part of `just check`, for `client-smoke`'s reason: it needs a GPU. The
# windowed half additionally needs a display, and is skipped rather than failed
# without one.
#
# Draws twenty frames headless and windowed, and fails if either process outlives
# its own run.
client-exit: (build "client")
    ./scripts/client-exit-test.sh ./{{build}}/client/client

# Run several scenes for five minutes and fail if the heap keeps climbing.
#
# **The check that turns "the client feels heavier after a while" into an exit
# code.** A leak is not a spike: it is a slope, and no frame profiler can show
# one because every individual frame looks fine. `core::HeapProfile` samples
# live bytes per tag once a second, fits a least-squares line to each tag over
# the run less its warm-up, and the client exits 3 naming any tag whose line
# both climbs faster than `limit` and actually *fits* one - a level load is a
# step, and a step is not a leak.
#
# **Five scenes at a minute each, and it is a minute of wall clock rather than
# of simulation.** Headless the client runs several thousand frames a second,
# so a minute here is a quarter of a million frames and an hour of ordinary
# play in frame terms, while the simulation still ticks a real 60 Hz. Both axes
# are exercised because leaks live on both: a per-frame one and a per-tick one
# are different bugs.
#
# The scenes are chosen to be different from each other rather than to be
# heavy - particles, portals, meshes, interface and physics allocate in
# different places, and a soak of one scene five times is one code path five
# times.
#
# **`--heap-warmup` is not slack in the limit, and raising the limit is not a
# substitute for it.** Content arrives over the first several seconds and the
# world settles after it; fitting a line through that start drags it through
# everything after. If a scene needs longer to settle, give it longer.
#
# Not part of `just check`, for `client-smoke`'s reason: it needs a GPU, and a
# build container with none would fail a check about memory for a reason that
# is not about memory.
#
# The reports are kept in `.cache/heap-<scene>.txt` whether the run passes or
# fails - a soak that went green is the baseline the next one is read against.
#
# Arguments are positional, as every recipe's are here - `seconds=300` after a
# recipe name is a string called "seconds=300", not a named argument, and the
# run that taught that lesson passed it to `--profile-seconds` and soaked for
# however long the timeout was.
#
#   just heap-soak                         # five scenes, a minute each
#   just heap-soak 300                     # five minutes each, so twenty-five
#   just heap-soak 60 8192 15 "Stress"     # one scene, everything spelled out
heap-soak seconds="60" limit="8192" warmup="15" scenes="Rings Particles Meshes Interface StressPhysics": (build "client")
    #!/usr/bin/env bash
    set -uo pipefail
    mkdir -p .cache
    failed=""
    for scene in {{scenes}}; do
        path="{{build}}/assets/examples/$scene.luau"
        if [ ! -f "$path" ]; then
            echo "FAIL: no staged scene at $path"
            exit 1
        fi
        report=".cache/heap-$scene.txt"
        echo "--- $scene: {{seconds}}s, failing above {{limit}} B/s after a {{warmup}}s warm-up"
        # Bounded, because this recipe cannot report a hang otherwise. The frame
        # budget is effectively unreachable; `--profile-seconds` is what ends it.
        timeout $(( {{seconds}} + 120 )) ./{{build}}/client/client \
            --headless --frames 1000000000 --profile-seconds {{seconds}} \
            --width 640 --height 360 --script "$path" \
            --heap-report "$report" --heap-growth-limit {{limit}} --heap-warmup {{warmup}} \
            > ".cache/heap-$scene.log" 2>&1
        status=$?
        if [ $status -eq 3 ]; then
            echo "LEAK in $scene:"
            grep "heap:" ".cache/heap-$scene.log" | tail -20
            failed="$failed $scene"
        elif [ $status -ne 0 ]; then
            echo "FAIL: $scene exited $status (124 means it never exited at all)"
            tail -20 ".cache/heap-$scene.log"
            failed="$failed $scene"
        else
            grep -h "heap: steady\|heap: .* live" ".cache/heap-$scene.log" | tail -2
        fi
    done
    if [ -n "$failed" ]; then
        echo "heap-soak FAILED:$failed - reports in .cache/heap-*.txt"
        exit 1
    fi
    echo "heap ok - every scene reached a steady state, reports in .cache/heap-*.txt"

# Drag the editor's window and check it is still alive afterwards.
#
# **The one bug class a headless run cannot reach.** The viewport shows last
# frame's scene texture, so resizing the panel means the renderer frees a
# texture the interface has already recorded a bind of - a use-after-free
# inside SDL's Vulkan backend, with nothing of ours on the stack. It needs a
# real window, a real swapchain and a window manager, which is exactly what
# `--headless` does not have.
#
# Not part of `just check`, for `studio-smoke`'s reason plus one more: it needs
# a display and `xdotool`, not only a GPU.
studio-resize: (build "studio")
    ./scripts/studio-resize-test.sh ./{{build}}/studio/studio

# Run the headless server. `just host --ticks 100` passes flags through.
host *args: (build "server")
    ./{{build}}/server/server {{args}}

# Public-domain PBR materials, downloaded and baked into the local content store.
#
# Two steps: the script writes source art into the store's `raw/`, and
# `contentimport --publish` bakes `raw/` into `baked/` and publishes that. It
# signs with `cdn::DevelopmentSigningKey` unless `--key` says otherwise, which is
# why no key appears here - see `cdn/LocalStore.hpp` for what that identity is
# and is not for.
#
# `just materials count=25` for a smaller pull. Re-running is cheap: the fetcher
# skips what is already on disk and the baker is deterministic.
materials count="100": (build "contentimport")
    python3 scripts/fetch-materials.py \
        --out ~/Documents/atomic-game-engine/cdn/raw --count {{count}}
    ./{{build}}/tools/contentimport --publish

# Run the content origin. `just serve --root ./content` passes flags through.
serve *args: (build "cdn")
    ./{{build}}/cdn/cdn {{args}}

# Every arrangement of a client and a server in one process.
#
# The diagnostic for "the replicated world is empty", and the place the modules
# are made to agree with each other. `--arrangement direct` cuts the handshake,
# the socket, the framing, the encryption and the bandwidth budget out of the
# middle and prints a column per stage, so the first column that stops making
# sense is the answer. The other eleven put those back one axis at a time and
# add content and discovery beside them.
#
# Every module's own report is printed underneath, and then the claims that
# span two of them - which is the part no module's suite can check, because no
# module links the other end of its own seams.
#
#   just unified                              # the bisection, one screen of it
#   just unified --all                        # all twelve, one line each
#   just unified --arrangement lossy+relayed  # content over a link that loses
#
# mono.unified_tests/AGENTS.md says how to read it.
unified *args: (build "unified_tests")
    ./{{build}}/unified_tests/unified_tests {{args}}

# Every arrangement, soaked, with the heap profiler watching.
#
# **`just heap-soak`'s question asked of the seams rather than of a scene.**
# That one runs the client on five scenes and asks whether drawing a world
# leaks; this runs every way the halves can be wired and asks whether *crossing*
# one does. They find different things: a leak in the relay's reassembly or in
# a session's retransmission buffer is invisible to a client that is not
# connected to anything, and is what this catches.
#
# **One process per arrangement, deliberately.** A single process running all
# twelve in turn has one heap history with twelve different workloads in it, and
# a slope fitted across that is fitted across the changeovers. Separate
# processes give each arrangement a history of its own.
#
# The arrangements come from the program rather than from a list here, so an
# axis added to `unified::Arrangement` is soaked without this recipe changing.
#
# Not part of `just check`: at the default it is five minutes, and a check
# somebody skips is a check that is not run.
#
# Arguments are positional, as every recipe's are here.
#
#   just unified-soak                # twelve arrangements, 25s each
#   just unified-soak 60             # twelve minutes
#   just unified-soak 30 4096 10     # tighter limit, longer warm-up
unified-soak seconds="25" limit="8192" warmup="8" entities="64": (build "unified_tests")
    #!/usr/bin/env bash
    set -uo pipefail
    mkdir -p .cache
    program="{{build}}/unified_tests/unified_tests"
    failed=""
    for arrangement in $("$program" --list-arrangements); do
        report=".cache/heap-unified-$arrangement.txt"
        log=".cache/heap-unified-$arrangement.log"
        echo "--- $arrangement: {{seconds}}s, failing above {{limit}} B/s after a {{warmup}}s warm-up"
        # Bounded, because this recipe cannot report a hang otherwise.
        timeout $(( {{seconds}} + 120 )) "$program" \
            --arrangement "$arrangement" --entities {{entities}} --seconds {{seconds}} --quiet \
            --heap-report "$report" --heap-growth-limit {{limit}} --heap-warmup {{warmup}} \
            > "$log" 2>&1
        status=$?
        if [ $status -eq 3 ]; then
            echo "LEAK in $arrangement:"
            grep "heap:" "$log" | tail -20
            failed="$failed $arrangement"
        elif [ $status -ne 0 ]; then
            echo "FAIL: $arrangement exited $status (124 means it never exited at all)"
            tail -20 "$log"
            failed="$failed $arrangement"
        else
            grep -h "heap: steady\|heap: .* live\|agrees with every other" "$log" | tail -3
        fi
    done
    if [ -n "$failed" ]; then
        echo "unified-soak FAILED:$failed - reports in .cache/heap-unified-*.txt"
        exit 1
    fi
    echo "unified ok - every arrangement reached a steady state, reports in .cache/heap-unified-*.txt"

# Two runs of one scene, compared byte for byte.
#
# The determinism guarantee, checked rather than claimed. A recording is one
# snapshot plus every envelope applied since, and both halves are written in a
# stable order - so two runs of the same scene produce identical files, and any
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
    @echo "determinism ok - {{ticks}} ticks over {{entities}} entities, byte-identical"
    @rm -f .cache/determinism-a.rec .cache/determinism-b.rec

# A recorded run, replayed, and the replay recorded again.
#
# Stronger than the above: it proves the *replay* path reproduces the run rather
# than merely that two live runs agree. A snapshot carries state and never code,
# so the replaying process registers the same systems - which it does by being
# the same program.
replay-check entities="256" ticks="120": (build "server")
    @rm -f .cache/replay-source.rec .cache/replay-again.rec
    ./{{build}}/server/server --entities {{entities}} --ticks {{ticks}} --unpaced         --record .cache/replay-source.rec > /dev/null
    ./{{build}}/server/server --replay .cache/replay-source.rec         --record .cache/replay-again.rec > /dev/null
    @test -f .cache/replay-again.rec         || (echo "FAIL: replaying with --record wrote nothing" && exit 1)
    @cmp .cache/replay-source.rec .cache/replay-again.rec         || (echo "FAIL: the replay did not reproduce the run it replayed" && exit 1)
    @echo "replay ok - {{ticks}} barriers reproduced, byte-identical"
    @rm -f .cache/replay-source.rec .cache/replay-again.rec

# Two hundred real clients against one server, and a flamegraph of what it cost.
#
# **Not part of `just check`.** It is a measurement rather than a gate: it takes
# a minute, its numbers depend on what else the machine is doing, and a gate that
# fails because somebody started a build is a gate people learn to ignore.
#
# The clients are genuine - a socket, a handshake, a cipher, an admission and a
# real `ecs::Store` each - and they all live in one process, because
# `client --headless` still builds a GPU device and two hundred of those measure
# the driver. `mono.tools/loadtest` is the harness and its `CMakeLists.txt` says
# why it is shaped that way.
#
# **Run it against `release` or `bench`.** First-party code is `-O0` under the
# default preset, so a tick cost from `just stress` is a number about the
# compiler. The recipe does not force the preset, for the reason every other
# recipe here does not: one override, applied before anything is derived.
#
#   just preset=release stress
#   just preset=release stress iter1 200 45
#
# Artefacts land in .cache/stress/ - the graph, the folded capture it came from,
# a greppable top-N, both logs, and the commit each was taken at.
stress label="baseline" clients="200" seconds="45" port="45100": (build "server") (build "loadtest")
    ./scripts/stress-test.sh {{build}} {{label}} {{clients}} {{seconds}} {{port}}

# Twenty thousand moving replicated parts and real encrypted clients. One client
# is enough to exercise the full authority-to-replica path without multiplying
# the object workload by a separate client-count experiment. Windowed every
# thirty ticks (one second at 30 Hz), so flamegraph.py --average has enough
# windows across a twenty-second run to make min/max/avg/median mean something.
stress-motion label="motion-baseline" clients="1" seconds="20" port="45200" window="30": (build "server") (build "loadtest")
    ./scripts/stress-test.sh {{build}} {{label}} {{clients}} {{seconds}} {{port}} ReplicationStress.luau {{window}}

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

# The origin on its own, which proves rather than asserts the claim: it
# configures and builds where there is no Vulkan SDK,
# no SDL and no shader compiler.
#
# MONO_VENDORED_GLSLC is left alone deliberately. The preset builds no client,
# so the root CMakeLists never resolves a glslc at all - and if that stops being
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

# There is one node graph in this repository and it is `mono.studio/nodegraph`.
#
# **The rule `D00113` spent two versions carrying.** That entry was open because
# this design existed twice - the editor's `studio/NodeGraph.hpp` and the
# template it came from - and the debt was never the second copy's quality. It
# is that both accumulate callers and the neglected one is the one that goes
# wrong, in the half nobody tests: the cycle guard, or the hash.
#
# A third would arrive as a file rather than as a decision. The render pipeline
# editor and `Engine::bakegraph`'s pipeline documents are both still to be
# written, and either could start its own registry and its own canvas without
# anybody noticing until the first divergence - which is exactly what AGENTS.md
# rule 6 says to make the build check rather than leave in somebody's memory.
#
# Extend the library where it lives. `mono.studio/nodegraph` is ours: it was a
# vendored submodule against a separate repository until v0.18.0, an engine
# module until v0.19, and a library of the editor's since - so extending it is
# an edit here rather than a pull request against somewhere else.
#
# The studio's Demo Nodes set is not a second implementation. It registers types
# through that module's public `NodeTypes::Register` and implements none of the
# model, which is the seam this rule is protecting rather than one it forbids.
check-one-node-graph:
    #!/usr/bin/env bash
    set -euo pipefail
    # `(engine::)?` because the namespace was `engine::nodegraph` between
    # v0.18.0 and v0.19 and is plain `nodegraph` again since the move to
    # `mono.studio`, and a second implementation would plausibly be spelled
    # either way. Its own directory is the one place the name is allowed.
    found=$(grep -rlE 'namespace[[:space:]]+(engine::)?nodegraph[[:space:]]*\{' \
        --include='*.hpp' --include='*.cpp' \
        mono.build mono.cdn mono.client mono.discord mono.engine mono.launcher mono.network \
        mono.server mono.studio mono.tools mono.unified_tests \
        | grep -v '^mono\.studio/nodegraph/' || true)
    if [ -n "$found" ]; then
        echo "FAIL: a second node graph implementation, in first-party code:"
        echo "$found" | sed 's/^/  /'
        echo "The one this repository has is mono.studio/nodegraph. Extend it there."
        exit 1
    fi
    echo "one node graph, and it is mono.studio/nodegraph"

# The API reference, from the comments already in the headers.
#
# mono.tools/docgen rewrites plain `//` into the `///` Doxygen reads, so nothing
# in the sources has to carry a marker. Needs doxygen on PATH; the target says
# so and how to get it if it is missing.
docs: (build "docgen")
    cmake --build --preset {{preset}} --target docs

# The generated site, on a local port. Python is not a prerequisite of this
# repository - it is only ever a convenience here, and the recipe says so rather
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
# `find .` so that mono.vendor/ is never touched - reformatting a submodule
# turns every future update into a conflict.
# Every directory holding first-party C++, and `mono.vendor` is the only one
# left out - their code, their style, and reformatting it would make every
# future bump a merge conflict.
#
# **`mono.studio` was missing until v0.18.0 and that was not deliberate.** It is
# the largest first-party module in the repository and it had never been
# formatted or checked, so the drift had been accumulating for as long as the
# list has existed: 722 violations across 49 files the first time it was asked.
# A list that is a subset of what it claims to cover is worse than a shorter
# claim, because `format-check ok` reads as "the tree is formatted".
mono_sources := "mono.engine mono.client mono.server mono.studio mono.unified_tests mono.cdn mono.launcher mono.network mono.discord mono.tools mono.build"

# Finding it is two problems, not one.
#
# `.clang-format` sets `BinPackParameters: OnePerLine`, which is an enum
# introduced in clang-format 21. Every older version reads that key as a boolean
# and stops with "invalid boolean" - a message that reads like the config is
# broken rather than the tool being too old, and which sent one person down the
# wrong path already.
#
# And installing clang-format-21 on Ubuntu leaves no unversioned `clang-format`
# on PATH, so looking for that name alone finds nothing on a machine that has
# exactly the right tool sitting next to it.
#
# So: search the versioned names too, and check the version rather than trusting
# whatever answers to the bare name.
#
# **The major is pinned, and "21 or newer" was the bug.** Two majors do not agree
# on the same file - include grouping and the wrapping of a long call are both
# places they differ - so a rule that accepted a range made the formatting a
# property of the machine. A box carrying an unversioned 21 *and* a versioned 23
# formatted with 21 because the bare name was tried first; a box with only 23
# formatted with 23; and the diff between them landed on files nobody had
# touched, which is what everybody was reverting by hand.
#
# One number, in one place, and an escape for somebody who knowingly wants
# another: `CLANG_FORMAT=clang-format-23 just format`. A deliberate override is a
# decision taken out loud, where "whatever answered first" is nobody's decision
# at all.
clang_format_major := "21"

find-clang-format := '''
    cf="${CLANG_FORMAT:-}"
    if [ -n "$cf" ]; then
        command -v "$cf" > /dev/null || { echo "CLANG_FORMAT=$cf is not on PATH." >&2; exit 1; }
    else
        for candidate in clang-format-$CLANG_FORMAT_MAJOR clang-format; do
            command -v "$candidate" > /dev/null || continue
            major=$("$candidate" --version | sed -nE 's/.*version ([0-9]+)\..*/\1/p')
            if [ "$major" = "$CLANG_FORMAT_MAJOR" ]; then cf="$candidate"; break; fi
        done
    fi
    if [ -z "$cf" ]; then
        echo "no clang-format $CLANG_FORMAT_MAJOR on PATH (.clang-format is written for it)." >&2
        echo "  install:  sudo apt install clang-format-$CLANG_FORMAT_MAJOR     # apt.llvm.org" >&2
        echo "  link it:  sudo update-alternatives --install /usr/bin/clang-format \\" >&2
        echo "                clang-format /usr/bin/clang-format-$CLANG_FORMAT_MAJOR 100" >&2
        echo "  or, deliberately:  CLANG_FORMAT=clang-format-NN just <recipe>" >&2
        exit 1
    fi
    cf_major=$("$cf" --version | sed -nE 's/.*version ([0-9]+)\..*/\1/p')
    if [ "$cf_major" != "$CLANG_FORMAT_MAJOR" ]; then
        echo "warning: formatting with clang-format $cf_major, not the pinned $CLANG_FORMAT_MAJOR." >&2
        echo "         expect files you did not touch to reflow." >&2
    fi
'''

format:
    #!/usr/bin/env bash
    set -euo pipefail
    # Exported rather than interpolated into `find-clang-format`: a `'''...'''`
    # is a raw string and Just substitutes once, into a recipe body.
    export CLANG_FORMAT_MAJOR={{clang_format_major}}
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
#
# **It names the tool it used, and the major is pinned so that name is the same
# everywhere.** `.clang-format` carries no version field - the tool has none to
# read - so `clang_format_major` above is where the number lives, and
# `find-clang-format` refuses anything else rather than taking whichever
# candidate answered first. That is what turns "two machines reformat the same
# file differently" from a thing to work around into a thing that cannot
# happen. `CLANG_FORMAT=` overrides it deliberately and warns when it does.
format-check:
    #!/usr/bin/env bash
    set -euo pipefail
    export CLANG_FORMAT_MAJOR={{clang_format_major}}
    {{find-clang-format}}
    echo "format-check with $cf $("$cf" --version | sed -nE 's/.*version ([0-9.]+).*/\1/p')" >&2
    find {{mono_sources}} \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
        | xargs -0 "$cf" --dry-run --Werror

# Every em dash in first-party prose. The root `AGENTS.md` bans them in capitals,
# and rule 6 there says a rule the build does not check is documentation.
#
# **It reports the character and deliberately offers no replacement.** An em dash
# is doing one of several different jobs - a parenthetical aside, the work of a
# colon, two sentences run together - and each one wants a different rewrite. A
# recipe that printed a substitute would be sed'd across the tree the first time
# somebody was in a hurry, and prose that has been sed'd reads worse than the
# prose it replaced. Rewrite the sentence.
#
# **U+2014 and nothing else.** An en dash spanning a range and a minus sign in
# front of a number are not violations, and `fixtures/clean.md` is full of both
# so that a scanner which started matching them fails here rather than sending
# somebody to rewrite `pages 10-14`.
#
# `mono.vendor/` is out for `format-check`'s reason: their code, their style.
# `mono.tools/emdash/` is out because one file in it holds a planted em dash on
# purpose, and `.claude/RESUME.md` is out because Claude Code writes it and would
# put the character straight back.
#
# **The seven files agents may not edit are in scope, and that is the point.**
# `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `README.md` and the three
# `docs/CODE_*.md` are the maintainer's own, so the sweep that added this recipe
# excluded them rather than risk editing one. They were measured at zero em
# dashes, so including them costs nothing today and closes the hole: the file
# that bans the character in capitals is now held to its own rule. If one ever
# reports, the fix belongs to whoever owns the file, and that is the correct
# place for it to land rather than in a list that quietly grows.
em-dash-check:
    #!/usr/bin/env bash
    set -euo pipefail

    # Spelled as bytes, so this recipe carries no em dash of its own and the
    # scan of the tree cannot match the scanner.
    dash=$(printf '\xe2\x80\x94')
    fixtures=mono.tools/emdash/fixtures

    # `/dev/null` on the end so grep prints a filename even when handed one file.
    scan() { grep -n -I -F -e "$dash" -- "$@" /dev/null || true; }

    # Untracked-but-not-ignored as well as tracked, so a file is checked before
    # it is added rather than after.
    mapfile -t files < <(
        git ls-files -c -o --exclude-standard -- \
            '*.cpp' '*.hpp' '*.luau' '*.ts' '*.cmake' '*.md' '*CMakeLists.txt' '*Justfile' \
        | grep -Ev '^(mono\.vendor/|mono\.tools/emdash/)' \
        | grep -Fxv '.claude/RESUME.md'
    )

    # --- the check checks itself first ------------------------------------
    #
    # A scan of nothing prints a tick, which is the one way this recipe could
    # be worse than not existing.
    if [ "${#files[@]}" -lt 1200 ]; then
        echo "em-dash-check FAILED - only ${#files[@]} file(s) in scope, so the file list is broken."
        exit 1
    fi
    # A here-string and not a pipe: `grep -q` leaves early, `printf` takes the
    # SIGPIPE, and `pipefail` would read that as the pattern being absent.
    listing=$(printf '%s\n' "${files[@]}")
    for want in '\.cpp$' '\.hpp$' '\.luau$' '\.ts$' '\.cmake$' '\.md$' 'CMakeLists\.txt$' 'Justfile$'; do
        if ! grep -qE "$want" <<< "$listing"; then
            echo "em-dash-check FAILED - nothing in scope matches /$want/, so the pathspec dropped it."
            exit 1
        fi
    done

    expected=$(cat "$fixtures/expect")
    planted=$(scan "$fixtures/planted.md")
    if ! grep -qF "$expected:" <<< "$planted"; then
        echo "em-dash-check FAILED - the planted em dash was not reported at $expected."
        printf '%s\n' "$planted" | sed 's/^/    /'
        exit 1
    fi
    if [ -n "$(scan "$fixtures/clean.md")" ]; then
        echo "em-dash-check FAILED - the clean fixture was reported, so this matches more than U+2014:"
        scan "$fixtures/clean.md" | sed 's/^/    /'
        exit 1
    fi

    # --- and then the tree --------------------------------------------------
    hits=$(scan "${files[@]}")
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed 's/^/  /'
        echo "em-dash-check FAILED - $(printf '%s\n' "$hits" | wc -l) em dash(es) above."
        echo "AGENTS.md: NEVER USE EM-DASHES. Rewrite the sentence, do not swap the character."
        exit 1
    fi
    echo "em-dash-check ok - no U+2014 in ${#files[@]} first-party file(s)"

clean:
    rm -rf .cache/build

# Everything derived, including the test cache.
clean-all:
    rm -rf .cache
