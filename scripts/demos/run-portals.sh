#!/usr/bin/env bash
#
# Holes in walls that lead somewhere the wall does not.
#
# Three rooms three hundred units apart and six portals between them: one pair
# that describes an ordinary adjacency, and two that put the same room through
# opposite walls of the one you are standing in.
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
