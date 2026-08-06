#!/usr/bin/env bash
#
# A `ScreenGui` built entirely from a script.
#
# `Instance.new` over the `gui` class tree, `UDim2` and `Vector2` as real
# property types, and layout that resolves against the window.
#
#   scripts/demos/run-interface.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-interface.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-interface.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-interface.sh        # no limit at all
#   PRESET=release scripts/demos/run-interface.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Interface.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
