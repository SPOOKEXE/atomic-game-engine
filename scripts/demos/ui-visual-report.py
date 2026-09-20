#!/usr/bin/env python3
"""Verify and present captures from ``ui-visual-check.sh``.

The checks intentionally read stable rendered regions instead of comparing a
full golden image. Font rasterisation differs between GPU drivers, while the
panel fill, its border and the checker texture are compositor contracts with
large enough regions to catch a missing GUI pass. Two click captures are then
compared with the no-click baseline in the counter and event-status regions.
They change when the script receives ``Activated`` and redraws its visible state.
"""

import html
import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - helper dependency
    sys.exit("PIL is required for UI visual checks. Install python3-pil.")

PANEL = (13, 16, 22)
BORDER = (76, 92, 115)
BLACK = (0, 0, 0)
CHECKER_MAGENTA = (206, 65, 111)
CHECKER_GRAY = (78, 78, 84)


def close(pixel, expected, tolerance=18):
    return max(abs(a - b) for a, b in zip(pixel[:3], expected)) <= tolerance


def count_close(image, box, colour, tolerance=18):
    return sum(close(pixel, colour, tolerance) for pixel in image.crop(box).getdata())


def panel_box(width, height):
    return ((width - 620) // 2, (height - 410) // 2, 620, 410)


def inspect(path):
    image = Image.open(path).convert("RGB")
    width, height = image.size
    left, top, panel_width, panel_height = panel_box(width, height)
    if width < 700 or height < 500:
        raise AssertionError(f"{path.name}: capture is too small for the fixed UI demo")

    panel = (left + 8, top + 8, left + 180, top + 100)
    checker = (left + 28, top + 122, left + 164, top + 258)
    # UIStroke centres itself on the panel edge, so the top row starts one
    # pixel outside the authored frame.
    border = (left - 1, top - 1, left + panel_width + 1, top + 1)
    panel_pixels = count_close(image, panel, PANEL)
    border_pixels = count_close(image, border, BORDER, 30)
    checker_pixels = list(image.crop(checker).getdata())
    magenta_checker = sum(close(pixel, CHECKER_MAGENTA) for pixel in checker_pixels)
    gray_checker = sum(close(pixel, CHECKER_GRAY) for pixel in checker_pixels)
    background = image.getpixel((8, 8))

    checks = {
        "black_background": close(background, BLACK),
        "panel_fill": panel_pixels >= 8000,
        "panel_border": border_pixels >= 300,
        "checker_magenta_cells": magenta_checker >= 3000,
        "checker_gray_cells": gray_checker >= 3000,
    }
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise AssertionError(
            f"{path.name}: failed {', '.join(failed)} "
            f"(panel={panel_pixels}, border={border_pixels}, checker magenta={magenta_checker}, gray={gray_checker})"
        )

    return {
        "file": path.name,
        "size": [width, height],
        "panel": [left, top, panel_width, panel_height],
        "panel_fill_pixels": panel_pixels,
        "border_pixels": border_pixels,
        "checker_magenta_pixels": magenta_checker,
        "checker_gray_pixels": gray_checker,
        "checks": checks,
    }


def changed_pixels(before, after, box):
    a = list(before.crop(box).convert("RGB").getdata())
    b = list(after.crop(box).convert("RGB").getdata())
    return sum(max(abs(x - y) for x, y in zip(one, two)) > 20 for one, two in zip(a, b))


def interaction_differences(before, after):
    width, height = before.size
    left, top, _, _ = panel_box(width, height)
    # Each comparison is restricted to the label it describes. It proves that
    # the visible result changed after the click, while a full-frame image stays
    # available for review instead of becoming a fragile golden assertion.
    counter = (left + 194, top + 118, left + 596, top + 150)
    event = (left + 194, top + 354, left + 596, top + 386)
    return {
        "counter": changed_pixels(before, after, counter),
        "event_status": changed_pixels(before, after, event),
    }


def write_report(directory, rows, deltas, review_rows):
    for row in rows + review_rows:
        preview = Path(row["file"]).with_suffix(".png").name
        Image.open(directory / row["file"]).convert("RGB").save(directory / preview)
        row["preview"] = preview
    report = {"captures": rows, "status_deltas": deltas}
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    figures = "\n".join(
        "<figure><a href=\"{file}\"><img src=\"{preview}\" alt=\"{file}\"></a>"
        "<figcaption>{file}: {size[0]}x{size[1]}, panel at {panel[0]},{panel[1]}</figcaption></figure>".format(
            **row
        )
        for row in rows
    )
    review_figures = "\n".join(
        "<figure><a href=\"{file}\"><img src=\"{preview}\" alt=\"{file}\"></a>"
        "<figcaption>{label}</figcaption></figure>".format(**row)
        for row in review_rows
    )
    delta_lines = "".join(
        f"<li>{html.escape(name)}: {counts['counter']} changed counter pixels, "
        f"{counts['event_status']} changed event-status pixels</li>" for name, counts in deltas.items()
    )
    (directory / "report.html").write_text(
        "<!doctype html><meta charset=\"utf-8\"><title>UI visual check</title>"
        "<style>body{background:#000;color:#fff;font:14px system-ui;margin:24px}"
        "main{max-width:1320px;margin:auto}figure{display:inline-block;vertical-align:top;width:48%;margin:1%}"
        "img{width:100%;image-rendering:auto;border:1px solid #4c5c73}figcaption{margin-top:6px;color:#b0bec5}</style>"
        "<main><h1>UI compositor check</h1><p>Solid fills, panel border, checker texture, and visible counter "
        "and event-status changes after each synthetic click passed. Open each image at full size for review.</p><ul>"
        + delta_lines
        + "</ul>"
        + figures
        + "<h2>Broader UI demo</h2><p>This still is for visual review. Its adaptive layout has no narrow "
        "pixel assertion.</p>"
        + review_figures
        + "</main>\n"
    )


def main(directory):
    directory = Path(directory)
    names = ("baseline-960", "baseline-1280", "text-button-1280", "image-button-1280")
    paths = {name: directory / f"{name}.bmp" for name in names}
    for path in paths.values():
        if not path.is_file():
            raise SystemExit(f"missing capture: {path}")

    rows = [inspect(paths[name]) for name in names]
    baseline = Image.open(paths["baseline-1280"]).convert("RGB")
    deltas = {}
    for name in ("text-button-1280", "image-button-1280"):
        changed = interaction_differences(baseline, Image.open(paths[name]).convert("RGB"))
        if changed["counter"] < 12 or changed["event_status"] < 12:
            raise AssertionError(
                f"{name}: click did not visibly update both counter and event status ({changed})"
            )
        deltas[name] = changed
    review_path = directory / "interface-1280.bmp"
    if not review_path.is_file():
        raise SystemExit(f"missing review capture: {review_path}")
    review_rows = [{"file": review_path.name, "label": "Interface.luau at 1280x720"}]
    write_report(directory, rows, deltas, review_rows)
    print(json.dumps({"status_deltas": deltas, "report": str(directory / "report.html")}, indent=2))


if __name__ == "__main__":
    main(sys.argv[1])
