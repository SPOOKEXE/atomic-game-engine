#!/usr/bin/env bash
#
# Holes in walls that lead somewhere the wall does not.
#
# A square building with four quarters and three rooms in it. Hall, library and
# garden clockwise round the middle, one door in the west wall, and a pair of
# holes where the fourth room would have been — so the lap closes after three
# right turns instead of four.
#
#   scripts/demos/run-portals.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-portals.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-portals.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-portals.sh        # no limit at all
#   PRESET=release scripts/demos/run-portals.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Portals-1-world.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
