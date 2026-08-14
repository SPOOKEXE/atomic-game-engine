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

# Every first-party object against the header dependencies ninja recorded for it.
#
# **The check for a bug that produced a working build and a broken binary.** At
# v0.15 fifty-seven of four hundred first-party objects had `#deps 0` — no
# recorded headers at all — so a change to a header rebuilt some translation
# units and not others, and the result held two different layouts of one struct.
# `studio::Options` grew a field, `Settings.ControlPort` read as zero instead of
# minus one in the objects that had not been rebuilt, and the studio died in
# `control::Server::Start` on a pimpl that was not there.
#
# **A full `cmake --build` does not find it and no test can.** Ninja believed
# everything was current, so the build was silent and green; the suites link
# whatever the objects say and cannot see that two of them disagree. That is the
# third category `AGENTS.md` rule 6 refuses to allow — a constraint that lives in
# nobody's memory — which is why this is a recipe rather than a paragraph.
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
        echo "deps-check FAILED — $missing of $total first-party object(s) track no headers."
        echo "A header change will not reach them, and the binary will mix struct layouts."
        exit 1
    fi
    echo "deps ok — $total first-party object(s) track their headers"

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

# Every authored script against the declarations, in both languages.
#
# **The other half of `bindings-check`.** That one asks whether the declarations
# still match the class table; this asks whether the scripts still match the
# declarations. A property removed from the class table regenerates cleanly and
# leaves every script that named it broken, with nothing reporting it until the
# scene fails to build.
#
# The Luau half is `mono.tools/scriptcheck`, which exists because upstream's
# `luau-analyze` has no way to load a definition file — see MonoVendor.cmake.
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
        echo "typecheck ok — luau. TypeScript skipped: no bun or npm on PATH."
        exit 0
    fi

    ./node_modules/.bin/tsc --noEmit
    echo "typecheck ok — luau and typescript $(./node_modules/.bin/tsc --version | cut -d' ' -f2)"

# The editor, with its control surface open for a Model Context Protocol client.
#
# **Two processes and one port.** This starts the editor listening on loopback;
# `mono.tools/mcpbridge` is what an MCP client launches, and it pumps bytes
# between the client's stdio and this port. `mono.studio/src/Control.cpp` carries
# why the editor listens rather than being launched.
#
# Off unless asked for: the surface runs scripts, writes properties and saves
# files for whatever connects, so opening it is a decision rather than a default.
# `just mcp` opens 8738 — the port `.mcp.json` and RUNNING.md name for the
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
# its own rather than a target. `just setup` walks past the submodule —
# `update = none` in `.gitmodules`, and the reason is written there — so this
# clones it on first run. Nothing else in the repository needs it: `just
# typecheck` gates on `mono.tools/scriptcheck`, which links our Luau.
#
# It is built in a tree of its own because it brings its own Luau and declares
# the same `Luau.*` target names ours does. Adding it to this build fails at
# configure time; `.gitmodules` carries the full argument.
#
# **`-Wno-error=maybe-uninitialized`, and the narrowness is the point.** 1.9.2
# hardcodes `-Wall -Werror` with no option to disable it, and GCC reports a
# false positive inside nlohmann/json's `NLOHMANN_DEFINE_TYPE_*` macros — so the
# build fails on a warning about vendored code in a vendored tree.
# `MonoVendor.cmake` turns Luau's own `LUAU_WERROR` off for exactly this reason.
#
# A blanket `-Wno-error` does not work here: `CMAKE_CXX_FLAGS` lands *before*
# their `target_compile_options`, so the later `-Werror` wins. A specific
# `-Wno-error=<warning>` takes precedence over the blanket form whatever the
# order, which is why this names the warning rather than silencing all of them —
# every other warning upstream cares about still fails the build.
#
# **`-include cstdint` is the second one, and it is a dated bug rather than a
# taste question.** luau-lsp 1.9.2 pins a Luau from before GCC 13 stopped
# including `<cstdint>` transitively, so `Ast/src/StringUtils.cpp` names
# `uint8_t` without including it and does not compile on a current toolchain.
# Forcing the header in is the standard answer and costs one flag; the
# alternative is editing a file inside two vendored trees.
#
# **This is the version skew `.gitmodules` warns about, arriving early.** Our own
# `mono.vendor/luau` is 0.732 and builds clean — the tree that does not is the
# one luau-lsp brought with it.
#
# Flags rather than patches, because `mono.vendor/AGENTS.md` says a patch goes
# upstream or into a fork, never into a file in this tree.
luau-lsp:
    #!/usr/bin/env bash
    set -euo pipefail
    git submodule update --init --recursive --checkout --depth 1 mono.vendor/luau-lsp

    # **The two Luaus must be one Luau.** The editor type-checks with the copy
    # luau-lsp brings and `just typecheck` with `mono.vendor/luau`; if they drift
    # apart, an author gets diagnostics from a language the engine does not run —
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

    cmake -S mono.vendor/luau-lsp -B .cache/build/luau-lsp -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="-Wno-error=maybe-uninitialized -include cstdint" > /dev/null
    cmake --build .cache/build/luau-lsp --target luau-lsp
    echo ""
    echo "luau-lsp built: $(pwd)/.cache/build/luau-lsp/luau-lsp"
    echo "Point your editor at it — see RUNNING.md, 'Autocomplete while you write one'."

# Every check there is, in the order to run them, against one preset.
#
# **The whole guarantee, and it is local and manual.** No machine other than
# this one runs any of it: there is no workflow on GitHub, and there will not be
# one until the repository's owner asks for it. `docs/DEFERRED.md` D00005 carries
# that decision and what it costs. So this recipe is not "what CI runs" — it is
# what a person runs before a push, and nothing runs it for them.
#
# The order is cheapest and most likely to fail first, so a misformatted file
# does not wait behind a compile.
#
# Not `preset=ci` by default, because that makes every warning fatal and the
# recipe is meant to be runnable mid-change. Use `just preset=ci check` for the
# strictest configuration this repository has.
check: format-check build test-all test-architecture bindings-check typecheck determinism replay-check
    @echo "check ok — format, build, tests, architecture, bindings, typecheck, determinism, replay"

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

# The editor, driven with no display at all.
#
# **What makes the studio checkable by something that is not a person.** It
# loads a game, starts it, renders the world into an offscreen target and writes
# the result — no window, no compositor, no machine that has to be left alone.
# A scripted control or an agent reads the image instead of a screen.
#
# Not part of `just check`: it needs a GPU, and a build container that has none
# would fail a check about the editor for a reason that is not about the editor.
# **And a second shot, of the scene built to show content.** Every headless check
# this editor could run was a count, and a count says the same thing whether the
# models are right, squashed, inside-out, untextured or culled away — all five of
# which have happened. The mesh grid is the one scene whose whole job is to make
# content visible, so photographing it turns "13 placed, 7 meshes, 16 textures"
# into something somebody can actually disagree with.
#
# **Longer than twelve frames, because content arrives over several.** Naming a
# mesh is what fetches it, the fetch spans frames, and the grid grows on
# `Heartbeat` as bundles land — so a capture at frame ten is a picture of an empty
# plate and says nothing.
studio-smoke game="" out=".cache/studio-smoke.bmp" meshes=".cache/studio-meshes.bmp": (build "studio")
    @rm -f {{out}} {{meshes}}
    ./{{build}}/studio/studio --headless --frames 12 --run play         {{ if game == "" { "" } else { "--game " + game } }}         --capture {{out}} --width 960 --height 540
    @test -s {{out}} || (echo "FAIL: the headless editor wrote no capture" && exit 1)
    ./{{build}}/studio/studio --headless --frames 700 --run play         --capture-world Assets --capture {{meshes}} --width 1280 --height 900
    @test -s {{meshes}} || (echo "FAIL: the headless editor wrote no mesh capture" && exit 1)
    @echo "studio ok — loaded, played and rendered with no display, into {{out}} and {{meshes}}"

# Press a button in a shipped client, with no display, and read the answer back.
#
# **The check `docs/DEFERRED.md` D00125 asked for, and the bug it names is why.**
# At v0.15 the shipped client did not route interface input at all — the router
# was constructed, read and never `Update`d, so a `TextButton` in a game never
# lit and its `Activated` never fired, while the same tree worked in the editor
# because the studio drives a router of its own. A button that does nothing in
# the shipped build and works in the editor is the worst version of that bug,
# because it is invisible to whoever is authoring.
#
# Closing it needed two things the client did not have: `--headless`, so the run
# needs no display, and `--click NAME`, so something can press. The press is
# synthesised into `input::Translator` as an ordinary SDL event and travels the
# path a real click travels — same translator, same `scene::InputState`, same
# `gui::Router`, same `Runtime::DeliverGuiEvents`. A click that took a shortcut
# past any of those would be a check of the shortcut.
#
# **It found a second one on its first run.** `examples::LoadScene` kept the only
# reference to the VM it created, so `RuntimeOf` answered null for a `--script`
# world and every gui event the router produced was delivered nowhere. The
# router was right, the events were right, and the last hop was missing.
#
# Not part of `just check`: it needs a GPU, for `studio-smoke`'s reason.
client-smoke: (build "client")
    #!/usr/bin/env bash
    set -euo pipefail
    scene="{{build}}/assets/examples/Interface.luau"
    test -f "$scene" || { echo "FAIL: no staged scene at $scene"; exit 1; }
    log=$(mktemp)
    trap 'rm -f "$log"' EXIT
    ./{{build}}/client/client --headless --frames 60 --width 960 --height 540 \
        --script "$scene" --click Swatch3 > "$log" 2>&1
    grep -q "click: pressed 'Swatch3'" "$log" \
        || { echo "FAIL: the client never found or pressed the button"; tail -20 "$log"; exit 1; }
    grep -q "interface: swatch 3 activated" "$log" \
        || { echo "FAIL: the button was pressed and its Activated never reached the script"; tail -20 "$log"; exit 1; }
    echo "client ok — pressed a button with no display and the script heard it"

# Drag the editor's window and check it is still alive afterwards.
#
# **The one bug class a headless run cannot reach.** The viewport shows last
# frame's scene texture, so resizing the panel means the renderer frees a
# texture the interface has already recorded a bind of — a use-after-free
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
# why no key appears here — see `cdn/LocalStore.hpp` for what that identity is
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
mono_sources := "mono.engine mono.client mono.server mono.unified_server_client mono.cdn mono.network mono.tools mono.build"

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
#
# **The major is pinned, and "21 or newer" was the bug.** Two majors do not agree
# on the same file — include grouping and the wrapping of a long call are both
# places they differ — so a rule that accepted a range made the formatting a
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
# everywhere.** `.clang-format` carries no version field — the tool has none to
# read — so `clang_format_major` above is where the number lives, and
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

clean:
    rm -rf .cache/build

# Everything derived, including the test cache.
clean-all:
    rm -rf .cache
