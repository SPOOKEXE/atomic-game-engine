#!/usr/bin/env bash
#
# Walks the eye across a portal pane's plane and reports what moves in the frame.
#
#   scripts/demos/seam-spill-sweep.sh                     # the near pane of PortalLightMix
#   AXIS=x AT=110.1 scripts/demos/seam-spill-sweep.sh     # the far pane of the same scene
#   SCENE=PortalSeam scripts/demos/seam-spill-sweep.sh    # somewhere else
#
# **A light field must not move when a player does, and this is how that is
# asked.** The renderer projects each portal mouth's far room back out of the
# mouth as light - `Renderer.cpp`'s seam light-field capture and
# `deferred-lighting.frag`'s `SeamSpill`. Which half-space the spill lands in is
# a fact about the doorway, so walking past the pane must change the picture only
# as much as walking past anything else does.
#
# What that looks like in the table is a column of small, similar steps. A term
# that reads the camera instead reads as one step many times its neighbours', at
# the sample where the eye crosses the pane, because `SeamSpill` drops every
# pixel with `depth <= 0.0` and so moves the whole pool from one side of the pane
# to the other in a single frame.
#
# **This is the instrument that caught one, and it has been watched firing.**
# The seam capture used to take the lit side from `cameraFrame.Position`, two
# lines copied from `subCameraFor` - where they are right, because that
# sub-camera really is placed from the eye. Run against that code on
# `PortalLightMix` this table reads a step of 9,195 pixels at the crossing,
# 14.9x the sweep's median of 616. With the viewer term gone the same sweep
# reads no step at all: a median of 824 and nothing above it.
#
# It is the ratio that is the reading and not the pixel count - exposure, scene
# and window size all move the second and none of them move the first.
#
# **The halves are the frame's, not the world's.** The eye looks along the pane
# rather than at it, so the pane's two sides fall either side of the picture and
# a pool changing sides shows up as a trade between the two columns rather than
# as a change in the total.
#
# `mono.client/tests/PortalLighting.cpp` asserts the other transport headlessly -
# the lamp copies `client::CollectLights` carries through a seam. This one needs
# a GPU, because the spill is a texture projected in a fragment shader and there
# is nothing on the CPU to read it back from.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/captures/seam-spill}

scene=${SCENE:-PortalLightMix}
frames=${FRAMES:-30}

# Which world axis the pane's normal lies along, and where along it the pane
# sits. The eye is placed off to one side and walks across that plane.
axis=${AXIS:-z}
at=${AT:--0.1}
standoff=${STANDOFF:--30}

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client > /dev/null

source="$build/assets/examples/$scene.luau"
if [ ! -f "$source" ]; then
	echo "no staged scene at $source" >&2
	exit 1
fi

rm -rf "$out"
mkdir -p "$out"

# **Evenly spaced, and that is what makes the table readable.** Every step is
# the same half stud, so the pixels a step moves can be compared directly with
# its neighbours' without dividing by anything. Unevenly spaced samples read a
# wide, smooth stretch as a jump and cry wolf.
#
# The pane falls in the middle of the -0.25 to +0.25 pair rather than on a
# sample, so the crossing is a step and not a point.
offsets=(-2.25 -1.75 -1.25 -0.75 -0.25 0.25 0.75 1.25 1.75 2.25)

for offset in "${offsets[@]}"; do
	staged="$out/$scene$offset.luau"
	step=$(python3 -c "print($at + $offset)")

	# **Appended rather than substituted**, so this works on any scene and not
	# only the ones with a `VIEW` line. Whatever camera the scene set is
	# replaced after it has finished building - a scene that drives its own
	# camera every frame overwrites this and reads a flat table.
	{
		cat "$source"
		if [ "$axis" = "x" ]; then
			printf '\nlocal __eye = Instance.new("Camera")\n__eye.Name = "SweepEye"\n__eye.FieldOfView = 70\n__eye.CFrame = CFrame.lookAt(Vector3.new(%s, 8, %s), Vector3.new(%s, 3, 0))\n__eye.Parent = workspace\nworkspace.CurrentCamera = __eye\n' \
				"$step" "$standoff" "$at"
		else
			printf '\nlocal __eye = Instance.new("Camera")\n__eye.Name = "SweepEye"\n__eye.FieldOfView = 70\n__eye.CFrame = CFrame.lookAt(Vector3.new(%s, 8, %s), Vector3.new(0, 3, %s))\n__eye.Parent = workspace\nworkspace.CurrentCamera = __eye\n' \
				"$standoff" "$step" "$at"
		fi
	} > "$staged"

	timeout --signal=KILL 180 "$build/client/client" \
		--script "$staged" --frames "$frames" \
		--capture "$out/$scene$offset.bmp" > /dev/null 2>&1 || true
done

python3 - "$out" "$scene" "$axis" <<'PY'
import glob
import os
import re
import sys

from PIL import Image

out, scene, axis = sys.argv[1], sys.argv[2], sys.argv[3]

# The dark a sunless portal scene defaults to, shared with
# `portal-lighting-report.py` so the two tools agree on what "lit" means.
DARK = 45


def halves(path):
    """Lit pixels either side of the frame's midline."""
    image = Image.open(path).convert("RGB")
    width, height = image.size
    pixels = image.load()
    left = right = 0
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            if r + g + b < DARK:
                continue
            if x < width // 2:
                left += 1
            else:
                right += 1
    return left, right


def offset_of(path):
    return float(re.sub(r"^.*?" + re.escape(scene), "", os.path.basename(path)).removesuffix(".bmp"))


rows = []
for shot in sorted(glob.glob(os.path.join(out, f"{scene}*.bmp")), key=offset_of):
    left, right = halves(shot)
    rows.append((offset_of(shot), left, right))

print()
print(f"{'eye ' + axis:>10s} {'lit-left':>10s} {'lit-right':>10s} {'step':>10s}")

steps = [abs(rows[i][1] - rows[i - 1][1]) for i in range(1, len(rows))]

# **A ratio against the rest of the sweep, not a fixed count.** How many pixels
# an ordinary step moves is a fact about the scene and the exposure; how much
# one step stands out from its evenly spaced neighbours is the thing being
# asked. Three is well above the spread a smooth sweep produces and well below
# the order of magnitude a dropped half-space does.
typical = sorted(steps)[len(steps) // 2] if steps else 0
STANDS_OUT = 3.0

for index, (offset, left, right) in enumerate(rows):
    if index == 0:
        print(f"{offset:>10.2f} {left:>10d} {right:>10d} {'-':>10s}")
        continue
    step = left - rows[index - 1][1]
    mark = ""
    if typical > 0 and abs(step) > typical * STANDS_OUT:
        mark = "  <- a step, not a walk"
    print(f"{offset:>10.2f} {left:>10d} {right:>10d} {step:>+10d}{mark}")

if typical > 0 and steps and max(steps) > typical * STANDS_OUT:
    print()
    print(f"  the largest step is {max(steps) / typical:.1f}x the sweep's median of {typical}")
    print("  something in the lighting is reading the camera; see this script's header")
else:
    print()
    print(f"  every step is within {STANDS_OUT:.0f}x the sweep's median of {typical} - nothing snaps")

print()
print(f"stills in {out}")
PY
