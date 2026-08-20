#!/usr/bin/env python3
"""Whether a mesh a script built actually reached the screen.

Two numbers per still, and only the pair means anything:

  built    pixels of the pyramid, which exists only as `scene::EditableMesh`
           arrays until something uploads them.

  control  pixels of the plain `Part` standing beside it, at the same size and
           the same distance.  It is what tells a missing upload apart from a
           camera pointed somewhere else: no control means the viewpoint moved
           and the run says nothing, and a control with no pyramid is the bug.

Run over the client's still and the editor's, in that order.  The editor drew
the control and never the pyramid for as long as it had no
`client::EditableMeshUploader` of its own.
"""

import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - a helper, not a dependency
    sys.exit("PIL is not installed; open the .bmp instead")

# The probe's two colours, matched by channel order rather than by value: a lit
# surface is its albedo times a light and never the flat number a scene names,
# but a red that stays redder than it is green survives any of them.
def built(pixel):
    red, green, blue = pixel[:3]
    return red > 70 and green < red * 0.55 and blue < red * 0.55


def control(pixel):
    red, green, blue = pixel[:3]
    return green > 70 and red < green * 0.55 and blue < green * 0.55


def main(paths):
    verdicts = []
    for path in paths:
        image = Image.open(path).convert("RGB")
        pixels = list(image.getdata())
        mesh = sum(1 for pixel in pixels if built(pixel))
        plain = sum(1 for pixel in pixels if control(pixel))

        print(f"  {path}")
        print(f"    built    {mesh:>7} px")
        print(f"    control  {plain:>7} px")

        if plain < 200:
            verdict = "NO CONTROL - the camera is not looking at the probe"
        elif mesh < 200:
            verdict = "MISSING - the script-built mesh never reached the device"
        else:
            verdict = "drew"
        print(f"    {verdict}")
        verdicts.append(verdict)

    return 0 if all(verdict == "drew" for verdict in verdicts) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
