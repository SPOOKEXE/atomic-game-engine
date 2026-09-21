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
CYAN = (92, 224, 232)
ORANGE = (255, 157, 78)
PURPLE = (190, 132, 255)
YELLOW = (255, 226, 92)


def close(pixel, expected, tolerance=18):
    return max(abs(a - b) for a, b in zip(pixel[:3], expected)) <= tolerance


def count_close(image, box, colour, tolerance=18):
    return sum(close(pixel, colour, tolerance) for pixel in image.crop(box).getdata())


def panel_box(width, height):
    return ((width - 620) // 2, (height - 410) // 2, 620, 410)


def responsive_panel_box(width, height):
    panel_width = round(width * 0.72)
    panel_height = round(height * 0.62)
    return ((width - panel_width) // 2, (height - panel_height) // 2, panel_width, panel_height)


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


def glyph_bounds(image, box, colour, tolerance=64):
    pixels = image.load()
    found = []
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            if close(pixels[x, y], colour, tolerance):
                found.append((x, y))
    if not found:
        return None
    xs, ys = zip(*found)
    return [min(xs), min(ys), max(xs) + 1, max(ys) + 1]


def inspect_text(path):
    image = Image.open(path).convert("RGB")
    width, height = image.size
    left, top, panel_width, panel_height = responsive_panel_box(width, height)
    right = left + panel_width
    bottom = top + panel_height
    if width < 700 or height < 500:
        raise AssertionError(f"{path.name}: capture is too small for the text UI demo")

    panel = (left + 8, top + 8, left + 120, top + 80)
    rich = (
        left + 28,
        top + round(panel_height * 0.20),
        right - 28,
        top + round(panel_height * 0.38),
    )
    regular = (
        left + 28,
        top + round(panel_height * 0.42),
        left + round(panel_width * 0.45) - 34,
        top + round(panel_height * 0.54),
    )
    code = (
        left + round(panel_width * 0.45),
        top + round(panel_height * 0.42),
        right - 28,
        top + round(panel_height * 0.54),
    )
    scaled = (left + 40, top + round(panel_height * 0.61) + 10, right - 40, bottom - 10)

    panel_pixels = count_close(image, panel, PANEL)
    cyan = glyph_bounds(image, rich, CYAN)
    orange = glyph_bounds(image, rich, ORANGE)
    purple = glyph_bounds(image, rich, PURPLE)
    regular_cyan = glyph_bounds(image, regular, CYAN)
    code_purple = glyph_bounds(image, code, PURPLE)
    scaled_yellow = glyph_bounds(image, scaled, YELLOW)
    checks = {
        "panel_fill": panel_pixels >= 4000,
        "rich_cyan_bold": cyan is not None,
        "rich_orange_italic": orange is not None,
        "rich_code_face": purple is not None,
        "regular_font_face": regular_cyan is not None,
        "code_font_face": code_purple is not None,
        "text_scaled": scaled_yellow is not None,
    }
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise AssertionError(f"{path.name}: failed {', '.join(failed)}")

    return {
        "file": path.name,
        "size": [width, height],
        "panel": [left, top, panel_width, panel_height],
        "panel_fill_pixels": panel_pixels,
        "rich_cyan_bounds": cyan,
        "rich_orange_bounds": orange,
        "rich_code_bounds": purple,
        "regular_font_bounds": regular_cyan,
        "code_font_bounds": code_purple,
        "scaled_bounds": scaled_yellow,
        "checks": checks,
    }


def scaled_growth(small, large):
    small_bounds = small["scaled_bounds"]
    large_bounds = large["scaled_bounds"]
    small_width = small_bounds[2] - small_bounds[0]
    small_height = small_bounds[3] - small_bounds[1]
    large_width = large_bounds[2] - large_bounds[0]
    large_height = large_bounds[3] - large_bounds[1]
    width_ratio = large_width / small_width
    height_ratio = large_height / small_height
    # The scene's box grows by 4/3. Leave room for font hinting while proving
    # that drawn glyphs, not just the panel, grew with the viewport.
    if width_ratio < 1.15 or height_ratio < 1.15:
        raise AssertionError(
            f"TextScaled glyphs did not grow with the viewport "
            f"(width={width_ratio:.3f}, height={height_ratio:.3f})"
        )
    return {"width_ratio": width_ratio, "height_ratio": height_ratio}


def font_shape_difference(row):
    regular = row["regular_font_bounds"]
    code = row["code_font_bounds"]
    regular_width = regular[2] - regular[0]
    code_width = code[2] - code[0]
    width_ratio = code_width / regular_width
    # The two labels draw the identical short string at the same point size.
    # A ratio close to one means the backend ignored Font and painted the same
    # glyph shapes twice. Inter and JetBrains Mono have distinct advance widths.
    if abs(width_ratio - 1.0) < 0.04:
        raise AssertionError(
            f"{row['file']}: regular and code glyph widths are too similar "
            f"({regular_width}px versus {code_width}px)"
        )
    return {"regular_width": regular_width, "code_width": code_width, "code_to_regular_width": width_ratio}


def write_report(directory, rows, deltas, review_rows, text_rows, text_growth, font_shapes):
    for row in rows + review_rows + text_rows:
        preview = Path(row["file"]).with_suffix(".png").name
        Image.open(directory / row["file"]).convert("RGB").save(directory / preview)
        row["preview"] = preview
    report = {
        "captures": rows,
        "status_deltas": deltas,
        "text_captures": text_rows,
        "text_scaled_growth": text_growth,
        "font_shape_difference": font_shapes,
    }
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
    text_figures = "\n".join(
        "<figure><a href=\"{file}\"><img src=\"{preview}\" alt=\"{file}\"></a>"
        "<figcaption>{file}: rich text colours, face labels, and TextScaled</figcaption></figure>".format(
            **row
        )
        for row in text_rows
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
        + "<h2>Text, rich text, and scaling</h2><p>Coloured rich-text runs and regular/code face labels all "
        "have visible glyph pixels. TextScaled glyph bounds grew from the 960px capture by "
        f"{text_growth['width_ratio']:.2f}x wide and {text_growth['height_ratio']:.2f}x tall at 1280px.</p>"
        + (
            f"<p>The same Sphinx 01 string is {font_shapes['text-1280']['regular_width']}px "
            f"in the regular face and {font_shapes['text-1280']['code_width']}px "
            "in the code face at 1280px.</p>"
        )
        + text_figures
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
    text_paths = [directory / "text-960.bmp", directory / "text-1280.bmp"]
    for path in text_paths:
        if not path.is_file():
            raise SystemExit(f"missing text capture: {path}")
    text_rows = [inspect_text(path) for path in text_paths]
    text_growth = scaled_growth(text_rows[0], text_rows[1])
    font_shapes = {
        Path(row["file"]).stem: font_shape_difference(row)
        for row in text_rows
    }
    write_report(directory, rows, deltas, review_rows, text_rows, text_growth, font_shapes)
    print(
        json.dumps(
            {
                "status_deltas": deltas,
                "text_scaled_growth": text_growth,
                "font_shape_difference": font_shapes,
                "report": str(directory / "report.html"),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main(sys.argv[1])
