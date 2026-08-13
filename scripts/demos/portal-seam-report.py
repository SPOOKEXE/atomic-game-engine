#!/usr/bin/env python3
"""What a still of `PortalSeam.luau` says about the seam.

Two numbers, one per view, so that a run in a terminal is worth something
without opening the file:

  side  the straddler's coloured pixels against the control's.  Both bars are
        the same size at the same depth, so a working cut roughly halves the
        straddler and leaves the control alone.

  far   how much of the near room's cyan lamp lands on the far room's red
        floor.  There is no lamp in the far room at all, so any blue there
        arrived through the hole.

`front` is the join, and it is a picture rather than a number: the two halves
of one bar meeting at the glass is the thing to look at.
"""

import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:  # pragma: no cover - a helper, not a dependency
    sys.exit("PIL is not installed; open the .bmp instead")

# The two authored colours, from `PortalSeam.luau`.  Matched by hue rather than
# by value, because a lit surface is its albedo times a light and a shadow and
# never the flat number the scene names.
STRADDLER = (255, 214, 0)
CONTROL = (120, 230, 120)


def classify(pixel):
    r, g, b = pixel[:3]
    total = r + g + b
    if total < 60:
        return None

    def near(colour):
        ct = sum(colour)
        return all(abs(a / total - c / ct) < 0.06 for a, c in zip((r, g, b), colour))

    if near(STRADDLER):
        return "straddler"
    if near(CONTROL):
        return "control"
    return None


def magenta(pixel):
    # The plume's authored colour, matched loosely: a particle is blended over
    # whatever is behind it, so its pixel is never the flat value.
    r, g, b = pixel[:3]
    return r > 90 and b > 70 and g < r * 0.7 and g < b * 0.8


def cut_report(image):
    counts = Counter()
    for pixel in image.getdata():
        found = classify(pixel)
        if found:
            counts[found] += 1

    straddler = counts["straddler"]
    control = counts["control"]
    print(f"    straddler {straddler:>7} px")
    print(f"    control   {control:>7} px")

    if control > 0:
        ratio = straddler / control
        verdict = "cut" if ratio < 0.75 else "WHOLE — the seam is drawing twice"
        print(f"    ratio     {ratio:>7.2f}  {verdict}")


def light_report(image):
    # The far room's floor, in front of its own pane, where a lamp that came
    # through the hole lands.  The floor is authored red, so blue is light.
    width, height = image.size
    patch = [
        image.getpixel((x, y))
        for x in range(int(width * 0.55), int(width * 0.70))
        for y in range(int(height * 0.65), int(height * 0.78))
    ]
    if not patch:
        return

    blue = sum(pixel[2] for pixel in patch) / len(patch)
    red = sum(pixel[0] for pixel in patch) / len(patch)
    print(f"    far floor  R {red:>5.1f}  B {blue:>5.1f}")

    verdict = "lit through the hole" if blue > 25.0 else "DARK — no light crossed"
    print(f"    {verdict}")

    # And the plume, which is the other thing only a hole can explain: the
    # emitter is in the near room and every spark here arrived through the pane.
    sparks = sum(1 for pixel in image.getdata() if magenta(pixel))
    print(f"    sparks    {sparks:>7} px")
    print(f"    {'carried through the hole' if sparks > 200 else 'NONE — particles die at the seam'}")


def main(path):
    image = Image.open(path).convert("RGB")
    print(f"  {path}")

    if path.endswith("far.bmp"):
        light_report(image)
    elif path.endswith("side.bmp"):
        cut_report(image)
    else:
        # **`front` is not counted, and pretending otherwise was worse than
        # saying nothing.** The control bar is beside the camera rather than
        # across the view there, so the two bars are at different depths and
        # their pixel counts are not a ratio of anything. What that view is for
        # is the join: one bar running into the glass and continuing inside the
        # picture, with the same colour on both sides of it.
        print("    the join — look at it rather than counting it")


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        main(argument)
