#!/usr/bin/env bash
#
# The shipped Luau libraries, loaded and exercised.
#
# `MagicCore` and `TerrainCore` unchanged from the Rojo project they came out of
# — not one line edited to make them run here.
#
#   scripts/demos/run-libraries.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-libraries.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-libraries.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-libraries.sh        # no limit at all
#   PRESET=release scripts/demos/run-libraries.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Libraries.luau"
SCENE_ARGS=()

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
