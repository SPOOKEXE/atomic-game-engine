#!/usr/bin/env bash
#
# Runs any staged Luau or TypeScript example through one launcher.
#
# Scene sources live in `mono.engine/examples`. TypeScript scenes are staged as
# JavaScript, so an explicit `.ts` name is translated to its emitted `.js` name.
# A bare stem means Luau. With no scene, the client opens Rings.luau.
#
#   scripts/demos/run-demo.sh Terrain --stats
#   scripts/demos/run-demo.sh Mirrors-1-world.ts --stats
#   scripts/demos/run-demo.sh Mirrors-4-worlds --worlds 4 --stats
#   MAX_FPS=60 scripts/demos/run-demo.sh Interface
#   PRESET=release scripts/demos/run-demo.sh Magic --stats
#
# Everything after the optional scene name reaches the client unchanged.

SCENE=""
SCENE_ARGS=()

if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
	SCENE=$1
	shift
	case "$SCENE" in
	*.luau | *.js) ;;
	*.ts) SCENE="${SCENE%.ts}.js" ;;
	*) SCENE="$SCENE.luau" ;;
	esac
fi

source "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
