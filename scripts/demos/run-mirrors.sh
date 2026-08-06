#!/usr/bin/env bash
#
# A room made of mirrors, and what it takes to make one.
#
# The rendering path: shadows, cameras that draw into a texture, and surfaces
# that sample the result a frame later.
#
#   scripts/demos/run-mirrors.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-mirrors.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-mirrors.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-mirrors.sh        # no limit at all
#   PRESET=release scripts/demos/run-mirrors.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Mirrors-1-world.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
