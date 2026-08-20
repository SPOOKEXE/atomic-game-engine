#!/usr/bin/env python3
"""What a still of the portal lighting scenes says about light crossing seams.

Both scenes turn the sun and both ambients off, so every pixel above the dark
threshold is one of the authored lamps - which is what makes counting hues a
measurement rather than a guess about exposure.

  PortalLightOut   one amber lamp standing in the near pane.  The `near` view
                   is the control (the lamp's own room must show a pool); the
                   `far` view is the claim: an amber pool in a room whose only
                   possible source is the copy carried through the seam.

  PortalLightMix   a red lamp on the near side, a green lamp on the far side.
                   Each view must show *both* channels - the room's own colour
                   directly, the other's through the hole - and a band where
                   the two overlap.  A view with one channel missing names
                   which copy was not made.

Verdicts print against fixed pixel-count thresholds, chosen well below a
working pool (tens of thousands of pixels at 1280x720) and well above noise.
"""

import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - a helper, not a dependency
    sys.exit("PIL is not installed; open the .bmp instead")

# Below this channel sum a pixel is the dark a sunless scene defaults to.
DARK = 45

# One channel has to lead another by this much to call a hue.
LEAD = 1.5

# A pool covers tens of thousands of pixels; this is an order below.
POOL = 2000


def survey(image):
    """Counts each hue a lamp could have thrown, over the whole frame."""
    counts = {"amber": 0, "red": 0, "green": 0, "mixed": 0, "lit": 0}
    for r, g, b in image.getdata():
        if r + g + b < DARK:
            continue
        counts["lit"] += 1

        # Amber is the one lamp `PortalLightOut` owns: warm, blue-starved.
        if r > b * 2.0 and g > b and r > g:
            counts["amber"] += 1

        # The mix scene's channels. "Mixed" is both present and neither
        # leading, which is the overlap band the roadmap line asks for.
        if r > g * LEAD and r > b * LEAD:
            counts["red"] += 1
        elif g > r * LEAD and g > b * LEAD:
            counts["green"] += 1
        elif r > b * LEAD and g > b * LEAD:
            counts["mixed"] += 1
    return counts


def out_report(view, counts, total):
    print(f"    amber {counts['amber']:>7} px    lit {counts['lit']:>7} of {total} px")
    if view == "near":
        verdict = "the lamp's own room is lit" if counts["amber"] > POOL else "DARK - the lamp itself is broken"
    else:
        verdict = "lit through the hole" if counts["amber"] > POOL else "DARK - no light crossed the seam"
    print(f"    {verdict}")

    # A sunless room must also still be mostly dark, or the pool is exposure.
    if counts["lit"] * 2 > total:
        print("    SUSPECT - over half the frame is lit in a sunless scene")


def mix_report(view, counts):
    own, crossed = ("red", "green") if view == "near" else ("green", "red")
    print(
        f"    red {counts['red']:>7} px   green {counts['green']:>7} px   "
        f"mixed {counts['mixed']:>7} px"
    )

    if counts[own] <= POOL:
        print(f"    NO {own.upper()} - this room's own lamp is broken (the control)")
    if counts[crossed] <= POOL and counts["mixed"] <= POOL:
        print(f"    NO {crossed.upper()} - the {crossed} copy never crossed the seam")
    if counts[own] > POOL and (counts[crossed] > POOL or counts["mixed"] > POOL):
        overlap = "and the pools mix" if counts["mixed"] > POOL else "but the pools do not overlap"
        print(f"    both lamps land in this room {overlap}")


def main(path):
    image = Image.open(path).convert("RGB")
    print(f"  {path}")

    name = path.rsplit("portal-lighting-", 1)[-1]
    scene, _, view = name.removesuffix(".bmp").rpartition("-")

    counts = survey(image)
    if scene.endswith("PortalLightOut"):
        out_report(view, counts, image.width * image.height)
    elif scene.endswith("PortalLightMix"):
        mix_report(view, counts)
    else:
        print(f"    not a portal lighting capture: {name}")


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        main(argument)
