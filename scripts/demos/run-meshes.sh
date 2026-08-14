#!/usr/bin/env bash
#
# Imported meshes, built-ins, textures, and a mirror.
#
# **Needs published content.** Without `--cdn` every mesh name resolves to the
# built-in fallback and the scene draws cubes - which is not a failure, and is
# not what it is for. `run-mesh-grid.sh` is the one that bakes and publishes;
# point this at a store you already have:
#
#   scripts/demos/run-meshes.sh --cdn dir:./store --publisher-key HEX
#
#   scripts/demos/run-meshes.sh                  # uncapped, held at 165 fps
#   scripts/demos/run-meshes.sh --graph          # extra flags reach the client
#   MAX_FPS=60 scripts/demos/run-meshes.sh       # hold a different rate
#   MAX_FPS=0 scripts/demos/run-meshes.sh        # no limit at all
#   PRESET=release scripts/demos/run-meshes.sh   # the shipped numbers instead
#
# Everything after the script name is appended to the client's own arguments, so
# `client --help` is the list of what may go there. RUNNING.md has the rest.

SCENE="Meshes.luau"
SCENE_ARGS=("--stats")

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
