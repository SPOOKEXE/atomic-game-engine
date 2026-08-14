#!/usr/bin/env bash
#
# A 16384 x 16384 noise-generated voxel world streamed around a camera.
#
# Chunks are generated on demand and evicted behind the camera, so the working
# set is a ring rather than a map. The view channel grows itself to fit -
# `--entities` seeds it and no longer caps it.
#
#   scripts/demos/run-terrain.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-terrain.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-terrain.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-terrain.sh        # no limit at all
#   PRESET=release scripts/demos/run-terrain.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Terrain.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
