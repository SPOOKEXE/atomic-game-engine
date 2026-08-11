#!/usr/bin/env bash
#
# Six rooms that lie about their own size.
#
# A row of exhibits, each one a hole onto a space that does not fit behind it:
# a tunnel shorter inside than out, one longer inside than out, a house with
# four doors onto three rooms, a pillar with two backs, a hill climbed by
# walking down, and a cell holding four times its own volume. The camera sweeps
# the row; hold the stats overlay to watch the twelve surface passes.
#
#   scripts/demos/run-non-euclidean.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-non-euclidean.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-non-euclidean.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-non-euclidean.sh        # no limit at all
#   PRESET=release scripts/demos/run-non-euclidean.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="NonEuclidean.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
