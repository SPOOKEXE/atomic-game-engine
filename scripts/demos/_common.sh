#!/usr/bin/env bash
#
# What every `run-*.sh` in this directory is made of.
#
# **Sourced, not executed.** Each scene script sets a few variables and then
# sources this; everything else - locating the repository, building, finding the
# staged client, the frame-rate policy - happens once, here. Eleven copies of a
# build sequence is eleven places to fix a preset name, and the copies would
# stop agreeing the first time one of them was edited in a hurry.
#
# A scene script sets, before sourcing:
#
#   SCENE       the example's filename, or empty for the built-in demo scene
#   SCENE_ARGS  an array of flags this scene needs to be itself
#
# Everything a caller passes on the command line is appended after those, so a
# scene's own flags can be overridden per run - the last spelling of an option
# is the one `core::Arguments` keeps.
#
# --- The frame-rate policy ------------------------------------------------
#
# **`--uncapped --max-fps 165`, and the pair is one decision rather than two.**
# `--uncapped` alone turns off the vblank wait and lets the loop run as fast as
# the GPU allows, which on a cheap scene is several hundred frames a second of
# heat for a display that shows a fraction of them. The vblank wait alone paces
# to whatever the display reports, which is not comparable between two machines
# and is not what a variable-refresh monitor does.
#
# So: do not wait for the display, and do not run away from it either. 165 is
# the rate these demos are tuned against; `MAX_FPS=0` removes the limit and
# `MAX_FPS=60` or any other number holds that instead.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"

# The server preset configures no client target at all, so `--target client`
# there fails inside CMake with `unknown target` and the reason is not in the
# message. Say what the preset is for instead.
if [ "$preset" = "server" ]; then
	echo "the 'server' preset builds no client - there is nothing to run." >&2
	echo "  try:  PRESET=dev $0" >&2
	exit 1
fi

if ! command -v cmake > /dev/null; then
	echo "cmake is not on PATH. CONTRIBUTING.md lists the prerequisites." >&2
	exit 1
fi

# Configure is quiet and the build is not. Re-running a demo should show what it
# is compiling, if anything, and nothing else - the configure has no news.
#
# The build is by directory rather than by preset, which is the one place these
# two lines are not symmetrical. `--build --preset` reads CMakePresets.json out
# of the working directory and has no `-S` to say otherwise, so the preset form
# works only from the repository root and fails with "Could not read presets
# from <wherever you were>" everywhere else.
cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client

client="$build/client/client"
if [ ! -x "$client" ]; then
	echo "built, but there is no client at $client" >&2
	echo "  the preset stages its programs somewhere this script does not" >&2
	echo "  expect. MONO_STAGE_ROOT in CMakeLists.txt is what decides." >&2
	exit 1
fi

pacing=(--uncapped)
if [ "${MAX_FPS:-165}" -gt 0 ]; then
	pacing+=(--max-fps "${MAX_FPS:-165}")
fi

scene=()
if [ -n "${SCENE:-}" ]; then
	# **The staged copy, not the source.** `mono.engine/examples/` is where a
	# scene is written and `assets/examples/` under the build is where it is
	# staged beside the binary it runs in - a demo that ran the source tree
	# would work here and nowhere a staged tree was copied to.
	staged="$build/client/assets/examples/$SCENE"
	if [ ! -f "$staged" ]; then
		staged="$build/assets/examples/$SCENE"
	fi
	if [ ! -f "$staged" ]; then
		echo "no staged scene at $staged" >&2
		echo "  the build did not stage $SCENE. Check mono.engine/examples/CMakeLists.txt." >&2
		exit 1
	fi
	scene=(--script "$staged")
fi

echo "running ${SCENE:-the built-in demo} at ${MAX_FPS:-165} fps"

# exec, so Ctrl-C and the exit status belong to the client rather than to a
# shell sitting in front of it.
exec "$client" "${scene[@]}" "${pacing[@]}" "${SCENE_ARGS[@]}" "$@"
