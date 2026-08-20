#!/usr/bin/env bash
#
# Bakes the pet-dance GIFs into a side content store, under the names the
# examples ask for.
#
#   scripts/demos/bake-fox-flipbook.sh
#   FOX_ART=~/Downloads/fox_pet scripts/demos/bake-fox-flipbook.sh
#   OUT=/tmp/foxstore scripts/demos/bake-fox-flipbook.sh
#
# **`Particles.luau` named `effects/fox_dance.atex` and nothing published it**,
# which is a shipped example whose one flipbook bay drew an untextured square.
# The GIF itself is in the local store - `contentimport` put it there - but
# `contentimport` renames every file it takes to its own content hash, so what
# the store holds is `67e91cae....atex` and no name in any scene can reach it.
# That is the right behaviour for a bulk import and the wrong one for an asset a
# demo refers to by name.
#
# `assetc` is the other half of the pipeline and it keeps the tree: a directory
# of art in, a directory of baked files out, under the same relative paths. So
# this stages the GIFs under the names the scenes use, bakes, and publishes the
# result as a store of its own.
#
# **A second store rather than an addition to the first**, and that is
# deliberate: the local store is a library somebody has filled by hand and
# re-publishing over it is a step this script has no business taking. A client
# takes `--cdn` more than once and stops at the first source that answers.
#
# Where the source GIFs come from, in order:
#
#   * `$FOX_ART`, a directory holding `fox_dance.gif` and friends;
#   * the local store's import log, which records what every hashed file in
#     `raw/` was called when it arrived. That is what makes this work on the
#     machine the GIFs were imported on without anybody keeping a copy.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"

store_root=${STORE_ROOT:-$HOME/Documents/atomic-game-engine/cdn}
out=${OUT:-$build/captures/fox-flipbook}

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target assetc > /dev/null
cmake --build "$build" --target cdn > /dev/null

art="$out/art/effects"
baked="$out/baked"
published="$out/store"

rm -rf "$out"
mkdir -p "$art" "$baked" "$published"

staged=0

if [ -n "${FOX_ART:-}" ] && [ -d "$FOX_ART" ]; then
	for gif in "$FOX_ART"/*.gif; do
		[ -e "$gif" ] || continue
		cp "$gif" "$art/$(basename "$gif")"
		staged=$((staged + 1))
	done
else
	log="$store_root/content.log"
	if [ ! -f "$log" ]; then
		echo "no $log and no FOX_ART - point FOX_ART at a folder of .gif files" >&2
		exit 2
	fi

	# The log is tab separated: when, what, the original path, the hash, bytes.
	# Only `import` rows name a file that is actually in `raw/` under that hash;
	# an `import-duplicate` row names one that was already there under another
	# name, and copying both would stage the same animation twice.
	while IFS=$'\t' read -r _when verb original hash _bytes; do
		[ "$verb" = "import" ] || continue
		case "$original" in
			*.gif) ;;
			*) continue ;;
		esac

		source="$store_root/raw/$hash.gif"
		[ -f "$source" ] || continue

		cp "$source" "$art/$(basename "$original")"
		staged=$((staged + 1))
	done < "$log"
fi

if [ "$staged" -eq 0 ]; then
	echo "no GIFs found to bake" >&2
	exit 1
fi

echo "staged $staged GIF(s) as effects/"

"$build/tools/assetc" --input "$out/art" --output "$baked" --quiet

# The development identity, spelled out in `cdn::DevelopmentSigningKey` and
# greppable there for exactly this reason: every tool that talks to a local
# store has to agree on it, and a store signed with a different one is a store
# the client refuses.
DEV_SEED=a70e1c9d54338b62f1402ad67718bc0593ee6f214cb80d7a35c2998650fb1348

"$build/cdn/cdn" --publish "$baked" --store "$published" --signing-key "$DEV_SEED"

echo
echo "published to $published"
echo "run a scene against it with:"
echo "  $build/client/client --cdn dir:$published --cdn dir:$store_root/processed \\"
echo "    --script $build/assets/examples/Particles.luau"
