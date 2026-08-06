#!/usr/bin/env bash
#
# Four worlds, four view producers, one composited frame.
#
# The view path *between* worlds: four independent simulations, each publishing
# to its own channel, composited into one frame. `--worlds 4` is what makes it
# four rather than one, and it is not optional.
#
#   scripts/demos/run-mirrors-4-worlds.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-mirrors-4-worlds.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-mirrors-4-worlds.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-mirrors-4-worlds.sh        # no limit at all
#   PRESET=release scripts/demos/run-mirrors-4-worlds.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Mirrors-4-worlds.luau"
SCENE_ARGS=("--worlds" "4" "--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
