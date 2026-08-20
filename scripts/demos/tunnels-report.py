#!/usr/bin/env python3
"""What a still of `Tunnels.luau` says about the walk down it.

Two numbers per view, measured in the box a walker looks through:

  prop    the share of that box taken by the drifting block.  A tunnel's whole
          demonstration is what is at the far end of it, so the block's colour
          here is a prop standing in the doorway rather than beside it.

  depth   the mean brightness of the same box.  It is the second, independent
          reading of one fact: a clear view down either tunnel ends in a dark
          room, and a blocked one ends a stud and a half away on a lit slab.

The colours matched are the *rendered* ones rather than the authored ones,
because a lit surface is its albedo times a light and never the flat number
the scene names - `LONG_MARK` is `(255, 107, 31)` in the file and reaches the
frame at roughly `(194, 136, 61)`.

`mono.engine/examples/tests/Scene.cpp` asserts the same claim as a volume and
headlessly: nothing stands in the middle of any walked space, and both blocks
are still in the scene.  This script is the half only a GPU can answer -
whether the view down the tunnel is actually clear.
"""

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - a helper, not a dependency
    sys.exit("PIL is not installed; open the .bmp instead")

# Each drifting block as the frame receives it, sampled from a capture of the
# scene's own lighting.
BLOCKS = {
    "long": (194, 136, 61),
    "short": (208, 249, 251),
}

# The box a walker looks through, as fractions of the frame.  Narrow on
# purpose: the floor stripes below it and the ceiling lamps above it are the
# same colours and both belong where they are, so a taller box would count the
# scene working as the scene failing.
CENTRE = (0.46, 0.40, 0.54, 0.60)

# Above this share of the box, the block is in the doorway rather than beside
# it.  A block that has just cleared the edge of the box contributes a few per
# cent; one filling it reads above two thirds.
BLOCKED = 0.25


def wears(pixel, colour):
    red, green, blue = pixel[:3]
    total = red + green + blue
    if total < 150:
        return False
    reference = sum(colour)
    return all(abs(a / total - c / reference) < 0.05 for a, c in zip((red, green, blue), colour))


def report(path, which):
    colour = BLOCKS[which]
    image = Image.open(path).convert("RGB")
    width, height = image.size

    left, top, right, bottom = CENTRE
    box = image.crop(
        (int(width * left), int(height * top), int(width * right), int(height * bottom))
    )
    pixels = list(box.getdata())

    prop = sum(1 for pixel in pixels if wears(pixel, colour)) / len(pixels)
    depth = sum(0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in pixels) / len(pixels)

    print(f"  {path}")
    print(f"    prop     {prop * 100:>6.1f}%")
    print(f"    depth    {depth:>6.1f} mean luma")
    print(
        "    BLOCKED - the drifting block is in the way of the walk"
        if prop > BLOCKED
        else "    clear - the block runs beside the walk rather than down it"
    )


def main(paths):
    for path in paths:
        stem = Path(path).stem
        which = next((name for name in BLOCKS if stem.endswith(name)), None)
        if which is None:
            # `plain` compares the two buildings from outside and has no walk in
            # it to be blocked.  It is a picture rather than a number, and
            # saying so beats counting something that means nothing.
            print(f"  {path}")
            print("    both buildings from the plain - look at it rather than counting it")
            continue
        report(path, which)


if __name__ == "__main__":
    main(sys.argv[1:])
