#!/usr/bin/env bash
#
# The demo, on Linux and macOS, from a clone that has run `just setup`.
#
# This is `just demo` for people who do not have `just`: it builds the client
# and runs the v0.1 demo scene with both debug panels open. It drives CMake
# directly rather than shelling out to `just`, because the Windows half of this
# pair (`run-demo.bat`) cannot assume `just` is installed and the two should not
# do different things.
#
#   scripts/run-demo.sh                      # dev preset, both panels
#   scripts/run-demo.sh --uncapped           # extra flags reach the client
#   scripts/run-demo.sh --frames 600 --entities 4000
#   PRESET=release scripts/run-demo.sh       # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments,
# so `client --help` is the list of what may go there. RUNNING.md has the rest.
#
# --- WHEN THE SCRIPTING RUNTIME LANDS (v0.5), THIS CHANGES ------------------
#
# The scene this runs is C++: `mono.client/src/Demo.cpp`, built into the client
# and selected by nothing. That is a stand-in. A game is meant to be a script,
# and once the Luau/TypeScript runtime exists the demo becomes a file the
# client is pointed at:
#
#   exec "$client" "$root/mono.engine/examples/Mirrors-1-world.luau" \
#       --stats --graph "$@"
#
# Swap the run line at the bottom for that when it works, and change nothing
# else — the build half of this script stays as it is. Two things have to land
# first: the VM and the bindings (ROADMAP.md v0.4-v0.5), and content in the
# example files, which are empty placeholders today. Until both are true the
# path above loads nothing — a bare positional path is collected by the parser
# and then ignored, with no message (RUNNING.md, "What happens today").
# `Demo.hpp` and `Demo.cpp` are deleted at that point; `mono.client/AGENTS.md`
# says so.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"

# The server preset configures no client target at all, so `--target client`
# there fails inside CMake with `unknown target`, and the reason for it is not
# in the message. It is not a mistake worth reading a build log over: say what
# the preset is for instead.
if [ "$preset" = "server" ]; then
    echo "the 'server' preset builds no client — there is nothing to demo." >&2
    echo "  try:  PRESET=dev $0" >&2
    exit 1
fi

if ! command -v cmake > /dev/null; then
    echo "cmake is not on PATH. CONTRIBUTING.md lists the prerequisites." >&2
    exit 1
fi

# Configure is quiet and the build is not. Re-running the demo should show what
# it is compiling, if anything, and nothing else — the configure has no news.
#
# The build is by directory rather than by preset, which is the one place these
# two lines are not symmetrical. `--build --preset` reads CMakePresets.json out
# of the working directory and has no `-S` to say otherwise, so the preset form
# works only when the demo is run from the repository root and fails with
# "Could not read presets from <wherever you were>" everywhere else. The
# directory is derived from the preset name anyway, and for a single-config
# generator that is the whole of what the build preset contributes.
cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client

client="$build/client/client"
if [ ! -x "$client" ]; then
    echo "built, but there is no client at $client" >&2
    echo "  the preset stages its programs somewhere this script does not" >&2
    echo "  expect. MONO_STAGE_ROOT in CMakeLists.txt is what decides." >&2
    exit 1
fi

echo "running the $preset demo: client --stats --graph $*"

# exec, so that Ctrl-C and the exit status belong to the client rather than to
# a shell sitting in front of it.
exec "$client" --stats --graph "$@"
