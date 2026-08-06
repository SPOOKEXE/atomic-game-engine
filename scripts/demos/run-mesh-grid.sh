#!/usr/bin/env bash
#
# The mesh grid, end to end: bake, publish, fetch, draw.
#
# `mono.engine/examples/MeshGrid.luau` needs content that has been baked and
# signed, so running it with `--script` alone shows nine fallback cubes. This
# does the three steps in front of it, with the flags that matter already set.
#
#   scripts/demos/run-mesh-grid.sh ART                 bake ART/ and run
#   scripts/demos/run-mesh-grid.sh ART --frames 600    extra flags reach the client
#   scripts/demos/run-mesh-grid.sh ART --capture g.bmp needs --frames as well
#   MAX_FPS=60 scripts/demos/run-mesh-grid.sh ART      hold a different rate
#
# `ART` is a directory of source art laid out however you like; the scene names
# the nine meshes it expects, so the paths under it have to match. The header of
# `MeshGrid.luau` lists them.
#
# **Blender files are not source art.** `.blend` is a format only Blender opens,
# so a model that exists only as one has to be exported to `.glb` first:
#
#   blender FILE.blend --background --python-expr \
#     'import bpy,sys; bpy.ops.export_scene.gltf(filepath="out.glb", \
#      export_format="GLB", export_apply=True, export_yup=True)'
#
# That carries meshes and *image* textures. A material built out of Blender's
# shader nodes is a procedure rather than an image and glTF has nowhere to put
# it, so those models arrive with base colours only — which is not a fault in
# the pipeline, and `MeshGrid.luau` puts one next to a textured model so the
# difference is visible rather than mysterious.
#
# Everything after the art directory is appended to the client's own arguments.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
preset="${PRESET:-dev}"
build="$root/.cache/build/$preset"

art="${1:-$root/ART}"
if [ $# -gt 0 ]; then shift; fi

if [ ! -d "$art" ]; then
	echo "run-mesh-grid: '$art' is not a directory" >&2
	echo "usage: scripts/demos/run-mesh-grid.sh ART [client flags...]" >&2
	exit 1
fi

work="${MESH_GRID_WORK:-$root/.cache/mesh-grid}"
content="$work/content"
store="$work/store"

cmake --build "$build" --target assetc cdn client

# **A throwaway key, because this signs nothing anybody else will verify.** A
# publisher's real seed belongs somewhere a repository is not; `RUNNING.md` says
# where. The public half is derived from it, so the client below can be pinned
# to the same identity without a second file.
signing="${MESH_GRID_KEY:-$(printf '7a%.0s' {1..32})}"

rm -rf "$content" "$store"

# **`--model-size 1`, and it is the flag this whole script exists to get right.**
# A `MeshPart`'s `Size` multiplies the mesh's own coordinates rather than fitting
# it into a box, so baking at the default of four and asking for a four-metre
# part draws sixteen metres and the grid overlaps itself. Unit meshes make an
# import behave exactly like a built-in cube.
"$build/tools/assetc" --input "$art" --output "$content" --no-copy --model-size 1

publisher=$("$build/cdn/cdn" --publish "$content" --store "$store" --signing-key "$signing" 2>&1 |
	sed -n 's/.*publisher key \([0-9a-f]*\).*/\1/p')

if [ -z "$publisher" ]; then
	echo "run-mesh-grid: the publish step printed no publisher key" >&2
	exit 1
fi

# `--entities` seeds the view channel and no longer caps it: a world that
# outgrows the reservation grows it in steps rather than being refused, so this
# number is a reservation rather than a ceiling somebody has to get right.
#
# **`--uncapped --max-fps` is one decision rather than two**, and every script in
# this directory makes the same one: do not wait for the display, and do not run
# away from it either. `_common.sh` carries the argument.
pacing=(--uncapped)
if [ "${MAX_FPS:-165}" -gt 0 ]; then
	pacing+=(--max-fps "${MAX_FPS:-165}")
fi

exec "$build/client/client" \
	--cdn "dir:$store" \
	--publisher-key "$publisher" \
	--content-cache "$work/cache" \
	--script "$root/mono.engine/examples/MeshGrid.luau" \
	--entities 2048 \
	"${pacing[@]}" \
	"$@"
