#!/usr/bin/env bash
#
# The built-in demo scene, which is C++ rather than a script.
#
# `mono.client/src/Demo.cpp`, selected by nothing and built into the client. It
# is the one thing here that is not a file the client is pointed at, which is
# why it has no `--script`.
#
#   scripts/demos/run-demo.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-demo.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-demo.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-demo.sh        # no limit at all
#   PRESET=release scripts/demos/run-demo.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE=""
SCENE_ARGS=()

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
