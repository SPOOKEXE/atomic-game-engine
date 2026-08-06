#!/usr/bin/env bash
#
# Rings of orbiting, spinning parts.
#
# The loading path: a script builds a world through the class table and animates
# it on `RunService.Heartbeat`. Written the way a Roblox script is written.
#
#   scripts/demos/run-rings.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-rings.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-rings.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-rings.sh        # no limit at all
#   PRESET=release scripts/demos/run-rings.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Rings.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
