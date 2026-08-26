#!/usr/bin/env bash
#
# The editor, on Linux and macOS, from a clone that has run `just setup`.
#
# This is `just edit` for people who do not have `just`: it builds the studio
# and opens it. It drives CMake directly rather than shelling out to `just`,
# because the Windows half of this pair (`run-studio.bat`) cannot assume `just`
# is installed and the two should not do different things.
#
#   scripts/run-studio.sh                          dev preset, empty editor
#   scripts/run-studio.sh --game My.agame          open a game at startup
#   scripts/run-studio.sh --run play               start playing rather than editing
#   PRESET=release scripts/run-studio.sh           the shipped numbers instead
#
# Everything after the script name is appended to the studio's own arguments,
# so `studio --help` is the list of what may go there. RUNNING.md has the rest.
#
# Two of those flags go together: `--headless` refuses to start without
# `--frames N`, because with no window there is nothing to close and no budget
# means never stopping. That pairing is what `just studio-smoke` runs, and it is
# the shape to use from anything that is not a person:
#
#   scripts/run-studio.sh --headless --frames 12 --run play --capture out.bmp
#
# This is the only program in the repository where `RunService:IsStudio()` is
# true, and the one that writes a `.agame`. `just host --game X.agame` and
# `just run --game X.agame` read the same file back.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"

# The studio is configured only where both halves exist: it needs the renderer
# to draw with and the server library to host "Play" with, so under the `server`
# preset the target is absent rather than stubbed (CMakeLists.txt, the
# MONO_BUILD_CLIENT AND MONO_BUILD_SERVER guard). `--target studio` there fails
# inside CMake with `unknown target`, and the reason for it is not in the
# message. Say what the preset is for instead.
if [ "$preset" = "server" ]; then
    echo "the 'server' preset builds no client - the studio needs one to draw with." >&2
    echo "  try:  PRESET=dev $0" >&2
    exit 1
fi

if ! command -v cmake > /dev/null; then
    echo "cmake is not on PATH. CONTRIBUTING.md lists the prerequisites." >&2
    exit 1
fi

# Configure is quiet and the build is not. Re-running the editor should show
# what it is compiling, if anything, and nothing else - the configure has no
# news.
#
# The build is by directory rather than by preset, which is the one place these
# two lines are not symmetrical. `--build --preset` reads CMakePresets.json out
# of the working directory and has no `-S` to say otherwise, so the preset form
# works only when the script is run from the repository root and fails with
# "Could not read presets from <wherever you were>" everywhere else. The
# directory is derived from the preset name anyway, and for a single-config
# generator that is the whole of what the build preset contributes.
cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target studio

studio="$build/studio/studio"
if [ ! -x "$studio" ]; then
    echo "built, but there is no studio at $studio" >&2
    echo "  the preset stages its programs somewhere this script does not" >&2
    echo "  expect. MONO_STAGE_ROOT in CMakeLists.txt is what decides." >&2
    exit 1
fi

echo "starting the $preset studio: studio $*"

# exec, so that Ctrl-C and the exit status belong to the studio rather than to a
# shell sitting in front of it.
exec "$studio" "$@"
