#!/usr/bin/env bash
#
# The ported libraries' own test suite, run by this engine's Luau.
#
# The parity check: 183 data tests that ran in Roblox Studio, run here. It prints
# results rather than drawing a scene, so there is nothing to look at.
#
#   scripts/demos/run-magic-tests.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-magic-tests.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-magic-tests.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-magic-tests.sh        # no limit at all
#   PRESET=release scripts/demos/run-magic-tests.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="MagicTests.luau"
SCENE_ARGS=()

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
