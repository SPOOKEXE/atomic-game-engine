#!/usr/bin/env bash
#
# A lattice of blocks in an empty sky, and nothing underneath.
#
# What a viewport has to survive: many small separate objects with sky visible
# between them, which is the case a depth-sorted renderer gets wrong quietly.
#
#   scripts/demos/run-skygrid.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-skygrid.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-skygrid.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-skygrid.sh        # no limit at all
#   PRESET=release scripts/demos/run-skygrid.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="SkyGrid.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
