#!/usr/bin/env bash
#
# Spells fired at generated terrain, which they dig holes in.
#
# The whole stack from data to a part on screen, through libraries ported from a
# Rojo project without an edit.
#
#   scripts/demos/run-magic.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-magic.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-magic.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-magic.sh        # no limit at all
#   PRESET=release scripts/demos/run-magic.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Magic.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
