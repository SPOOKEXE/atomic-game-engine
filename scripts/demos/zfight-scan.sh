#!/usr/bin/env bash
#
# Photographs a scene twice from cameras a fraction of a stud apart and reports
# how much of the frame changed colour.
#
#   scripts/demos/zfight-scan.sh                       # every portal and mirror scene
#   scripts/demos/zfight-scan.sh Portals-1-world       # one scene
#   FRAMES=90 scripts/demos/zfight-scan.sh Tunnels     # later in the scene's own motion
#   NUDGE=0.05 scripts/demos/zfight-scan.sh            # a coarser move
#
# **Z-fighting is a thing a still cannot show and a diff can.** Two coplanar
# surfaces at one depth swap which of them wins on a change to the projection too
# small to move anything else - so the same frame taken from a camera nudged by a
# thousandth of a stud is identical everywhere except where the fight is, and
# there it flips wholesale between two surface colours. Everything that is merely
# *geometry* moves by well under a pixel and diffs to nothing.
#
# **The difference is eroded before it is counted, and without that the tool
# lies.** A sub-pixel shift moves every silhouette in the scene by a fraction of
# a pixel, so a frame full of straight edges - which a room made of tiled walls
# is - lights up a one-pixel outline around every one of them.
# `Portals-1-world` reads 1.26% of the frame that way and not one pixel of it is
# a fight. Dropping anything within one pixel of unchanged ground removes every
# outline and leaves only regions that changed *through*, which is what two
# surfaces swapping looks like and what an edge can never be.
#
# **What the control found, and it changes how to read the whole table.** Two
# slabs with their top faces at exactly y = 0 do *not* flicker in this engine:
# they come out a uniform blue, `FloorB` winning every pixel, with no red
# anywhere and no stripe at any distance. The depth test resolves the tie the
# same way every frame and the draw order does not depend on the projection, so
# coplanar geometry here is stably wrong rather than noisily wrong.
#
# So a zero in this table means "nothing swaps as the eye moves" and does *not*
# mean "nothing is coplanar" - the second question needs geometry, not pictures.
# What this tool does catch is anything whose visibility genuinely turns on the
# viewpoint: a sorted transparent run changing order, a surface pass picking a
# different winner, a cull that flips.
#
# **A mirror is a false positive and is left in the list on purpose.** What a
# pane shows is rendered from a camera derived from the eye, so nudging the eye
# genuinely changes the image in the pane. At the default nudge that change is
# sub-pixel and the erosion removes it; at `NUDGE=0.5` it is real and reads.
#
# The camera is moved after the scene has built itself, so a scene that drives
# its own camera every frame overwrites the nudge and reads zero. Those are the
# ones to check by hand.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/captures/zfight}

frames=${FRAMES:-30}
nudge=${NUDGE:-0.0015}

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client > /dev/null

scenes=("$@")
if [ ${#scenes[@]} -eq 0 ]; then
	scenes=(
		Portals-1-world Tunnels PortalSeam PortalShadow PortalProbe
		PortalLightMix PortalLightOut CrossWorldSeam ImmersivePortals
		NonEuclidean MirrorCorridor MirrorDepth Mirrors-1-world StressMirrors
	)
fi

rm -rf "$out"
mkdir -p "$out"

# **A scene that is deliberately wrong, scanned alongside the real ones.**
# A detector nobody has ever seen fire is a detector that reads zero for both
# "nothing is wrong" and "this cannot see anything", and those are the two
# answers it exists to tell apart. Two slabs at exactly one height are the fault
# in its simplest form. The second control capture moves the winner down by a
# hundredth of a stud, forcing the other coplanar face to win. If that region is
# not detected, the run says nothing about the scenes under it.
cat > "$out/zfight-control.luau" <<'CONTROL'
local function slab(name, colour, z)
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = Vector3.new(80, 1, 80)
	part.Position = Vector3.new(0, -0.5, z)
	part.Color = colour
	part.Parent = workspace
	return part
end

-- Two floors with their top faces at exactly y = 0 and nothing to separate
-- them. This is the fault the editor's own comments warn about when a template
-- lays a baseplate under a scene that lays its own floor.
slab("FloorA", Color3.fromRGB(220, 60, 60), 0)
slab("FloorB", Color3.fromRGB(60, 60, 220), 0)

local eye = Instance.new("Camera")
eye.Name = "Viewer"
eye.FieldOfView = 60
eye.CFrame = CFrame.lookAt(Vector3.new(0, 6, 34), Vector3.new(0, 0, 0))
eye.Parent = workspace
workspace.CurrentCamera = eye
CONTROL

scenes=("zfight-control" "${scenes[@]}")

for scene in "${scenes[@]}"; do
	source="$build/assets/examples/$scene.luau"
	if [ "$scene" = "zfight-control" ]; then
		source="$out/zfight-control.luau"
	fi
	if [ ! -f "$source" ]; then
		echo "  skip $scene - no staged scene at $source" >&2
		continue
	fi

	if [ "$source" != "$out/$scene-a.luau" ]; then
		cp "$source" "$out/$scene-a.luau"
	fi

	# **Composed rather than added**, because a `CFrame` plus a `Vector3` is not
	# a thing: the move has to be in the camera's own space or a scene looking
	# along -Z gets nudged sideways instead of up.
	{
		cat "$source"
		printf '\nlocal __cam = workspace.CurrentCamera\nif __cam then\n\t__cam.CFrame = __cam.CFrame * CFrame.new(0, %s, 0)\nend\n' "$nudge"
		if [ "$scene" = "zfight-control" ]; then
			printf 'workspace.FloorB.Position = workspace.FloorB.Position - Vector3.new(0, 0.01, 0)\n'
		fi
	} > "$out/$scene-b.luau"

	for half in a b; do
		timeout --signal=KILL 120 "$build/client/client" \
			--script "$out/$scene-$half.luau" --frames "$frames" \
			--capture "$out/$scene-$half.bmp" > /dev/null 2>&1 || true
	done
done

python3 - "$out" <<'PY'
import glob
import os
import sys

from PIL import Image, ImageChops, ImageFilter

out = sys.argv[1]
rows = []

for first in sorted(glob.glob(os.path.join(out, "*-a.bmp"))):
    second = first[:-6] + "-b.bmp"
    name = os.path.basename(first)[:-6]
    if not os.path.exists(second):
        rows.append((name, None))
        continue

    before = Image.open(first).convert("RGB")
    after = Image.open(second).convert("RGB")
    pixels = list(ImageChops.difference(before, after).getdata())

    # Sixty of a possible 765, which is past anything a sub-pixel shift can do
    # to a shaded surface and well under a swap between two different ones.
    changed = Image.new("L", before.size)
    changed.putdata([255 if r + g + b > 60 else 0 for r, g, b in pixels])

    # **Eroded by one pixel**, which is what separates an outline from a
    # region - see the header. A `MinFilter` of three keeps a pixel only where
    # all eight of its neighbours changed too.
    solid = changed.filter(ImageFilter.MinFilter(3))

    flipped = sum(1 for value in solid.getdata() if value > 0)
    rows.append((name, 100.0 * flipped / len(pixels)))

print()
print(f"{'scene':24s} {'flipped':>8s}")
for name, share in rows:
    if share is None:
        print(f"{name:24s} {'no capture':>8s}")
        continue
    mark = "  <- look at this one" if share >= 0.25 else ""
    print(f"{name:24s} {share:7.2f}%{mark}")
PY

echo
echo "stills in $out"
