#!/usr/bin/env python3
"""Exercise UiLab through a real X11 client window and retain visual evidence.

Run after building and staging the client: ``python3 scripts/demos/ui-live-check.py``.
The MCP connection only reads the scene and sends UTF-8 text. Pointer and key
actions use X11, so the checks exercise the same window event route as a user.
Requires Pillow, xdotool, xwininfo, xrandr, xprop, and wmctrl on an X11 desktop.
"""

from __future__ import annotations

import argparse
import html
import json
import math
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    from PIL import Image, ImageChops, ImageGrab
except ImportError:
    sys.exit("Pillow is required: install python3-pil")


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SIZES = ((360, 640), (640, 360), (960, 540), (1280, 720), (1920, 1080))
REQUIRED_NAMES = (
    "LabScreen",
    "Backdrop",
    "Content",
    "ClickButton",
    "ImageButton",
    "Entry",
    "Scale075",
    "Scale100",
    "Scale150",
    "ScaleSample",
    "ScaleControl",
    "AspectSample",
    "ScrollList",
    "ScrollRow1",
    "ScrollRow12",
    "Selected",
    "DragArea",
    "DragTarget",
    "ToggleVisibility",
    "VisibilityTarget",
    "CreateItem",
    "DestroyItem",
    "Items",
    "Reset",
    "Feedback",
    "Status",
)


@dataclass(slots=True)
class Rect:
    x: float
    y: float
    width: float
    height: float

    @property
    def center(self) -> tuple[int, int]:
        return round(self.x + self.width / 2), round(self.y + self.height / 2)

    def intersection(self, other: Rect) -> Rect:
        left = max(self.x, other.x)
        top = max(self.y, other.y)
        right = min(self.x + self.width, other.x + other.width)
        bottom = min(self.y + self.height, other.y + other.height)
        return Rect(left, top, max(0, right - left), max(0, bottom - top))


class Mcp:
    def __init__(self, port: int):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.socket.settimeout(5)
        self.reader = self.socket.makefile("rb")
        self.request_id = 0
        self.request("initialize", {})

    def close(self) -> None:
        self.reader.close()
        self.socket.close()

    def request(self, method: str, params: dict[str, Any]) -> Any:
        return self.request_many([(method, params)])[0]

    def request_many(self, calls: list[tuple[str, dict[str, Any]]]) -> list[Any]:
        requests = []
        for method, params in calls:
            self.request_id += 1
            requests.append(
                {
                    "jsonrpc": "2.0",
                    "id": self.request_id,
                    "method": method,
                    "params": params,
                }
            )
        self.socket.sendall(
            "".join(
                json.dumps(request, separators=(",", ":")) + "\n"
                for request in requests
            ).encode()
        )
        replies: dict[int, Any] = {}
        expected_ids = {request["id"] for request in requests}
        for _ in requests:
            line = self.reader.readline()
            if not line:
                raise RuntimeError("MCP disconnected during batched requests")
            reply = json.loads(line)
            reply_id = reply.get("id")
            if reply_id not in expected_ids or reply_id in replies or "error" in reply:
                raise RuntimeError(f"MCP batch reply: {reply}")
            replies[reply_id] = reply["result"]
        return [replies[request["id"]] for request in requests]

    def tool(self, name: str, **arguments: Any) -> Any:
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        return self._tool_result(name, result)

    def tool_batch(self, calls: list[tuple[str, dict[str, Any]]]) -> list[Any]:
        results = self.request_many(
            [
                ("tools/call", {"name": name, "arguments": arguments})
                for name, arguments in calls
            ]
        )
        return [
            self._tool_result(name, result)
            for (name, _), result in zip(calls, results, strict=True)
        ]

    @staticmethod
    def _tool_result(name: str, result: dict[str, Any]) -> Any:
        if result.get("isError"):
            raise RuntimeError(f"MCP {name}: {result['content']}")
        content = result.get("content", [])
        if not content or content[0].get("type") != "text":
            raise RuntimeError(f"MCP {name} returned no text: {result}")
        return json.loads(content[0]["text"])


def command(*arguments: str, timeout: float = 5) -> str:
    result = subprocess.run(
        arguments, text=True, capture_output=True, timeout=timeout, check=True
    )
    return result.stdout.strip()


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_until(
    description: str, predicate: Callable[[], Any], seconds: float = 4
) -> Any:
    deadline = time.monotonic() + seconds
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, RuntimeError, ValueError) as error:
            last_error = error
        time.sleep(0.05)
    detail = f": {last_error}" if last_error else ""
    raise RuntimeError(f"timed out waiting for {description}{detail}")


def child_window(pid: int) -> str | None:
    result = subprocess.run(
        ["xdotool", "search", "--onlyvisible", "--pid", str(pid)],
        text=True,
        capture_output=True,
        check=False,
    )
    for window in result.stdout.splitlines():
        owner = subprocess.run(
            ["xdotool", "getwindowpid", window],
            text=True,
            capture_output=True,
            check=False,
        )
        if owner.returncode == 0 and owner.stdout.strip() == str(pid):
            return window
    return None


def window_rect(window: str) -> Rect:
    output = command("xwininfo", "-id", window)

    def number(label: str) -> int:
        found = re.search(rf"^\s*{re.escape(label)}:\s*(-?\d+)", output, re.MULTILINE)
        if found is None:
            raise RuntimeError(f"xwininfo omitted {label}")
        return int(found.group(1))

    return Rect(
        number("Absolute upper-left X"),
        number("Absolute upper-left Y"),
        number("Width"),
        number("Height"),
    )


def display_origin(sizes: list[tuple[int, int]]) -> tuple[int, int]:
    widest = max(width for width, _ in sizes)
    tallest = max(height for _, height in sizes)
    output = command("xrandr", "--current")
    monitors = [
        tuple(map(int, match.groups()))
        for match in re.finditer(
            r"^\S+ connected (?:primary )?(\d+)x(\d+)\+(-?\d+)\+(-?\d+)",
            output,
            re.MULTILINE,
        )
    ]
    fitting = [
        (width * height, x, y)
        for width, height, x, y in monitors
        if width >= widest and height >= tallest
    ]
    if not fitting:
        raise RuntimeError(f"no connected X11 monitor fits {widest}x{tallest}")
    _, x, y = min(fitting)
    return x, y


def clear_previous_report(output: Path) -> None:
    report = output / "report.json"
    try:
        previous = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        previous = {}
    if not isinstance(previous, dict):
        previous = {}
    for capture in previous.get("captures", []):
        name = capture.get("file") if isinstance(capture, dict) else None
        if isinstance(name, str) and re.fullmatch(
            r"(?:baseline-\d+x\d+|button-(?:\d+x\d+|idle|hover|pressed)|scale-(?:075|100|150)|created-item-click|failure-\d+|final-before-profile|fatal)\.png",
            name,
        ):
            (output / name).unlink(missing_ok=True)
    for name in (
        "report.json",
        "report.html",
        "client.log",
        "profile-live.json",
        "frame-profile.json",
        "frame-profile.txt",
    ):
        (output / name).unlink(missing_ok=True)


def place_window(window: str, origin: tuple[int, int]) -> None:
    # Keep the native SDL window and input path, but remove the manager's frame
    # so a 1920x1080 client can fit a 1920x1080 monitor pixel for pixel.
    command(
        "xprop",
        "-id",
        window,
        "-f",
        "_MOTIF_WM_HINTS",
        "32c",
        "-set",
        "_MOTIF_WM_HINTS",
        "2, 0, 0, 0, 0",
    )
    command("xdotool", "windowmove", "--sync", window, str(origin[0]), str(origin[1]))
    time.sleep(0.12)


def descendants(node: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        node,
        *(
            member
            for child in node.get("children", [])
            for member in descendants(child)
        ),
    ]


def status_values(value: str) -> dict[str, Any]:
    value = value.strip()
    if value.startswith("{"):
        decoded = json.loads(value)
        if isinstance(decoded, dict):
            return decoded
    values: dict[str, Any] = {}
    for key, raw in re.findall(
        r'\b([A-Za-z][A-Za-z0-9]*)\s*=\s*("(?:\\.|[^"\\])*"|[^\s|,;]+)', value
    ):
        if raw.lower() in ("true", "false"):
            values[key] = raw.lower() == "true"
        elif re.fullmatch(r"-?\d+", raw):
            values[key] = int(raw)
        elif re.fullmatch(r"-?\d+(?:\.\d+)?", raw):
            values[key] = float(raw)
        elif raw.startswith('"'):
            values[key] = json.loads(raw)
        else:
            values[key] = raw
    return values


class Lab:
    def __init__(self, mcp: Mcp, window: str, output: Path, deadline: float):
        self.mcp = mcp
        self.window = window
        self.output = output
        self.deadline = deadline
        self.world = ""
        self.nodes: dict[str, dict[str, Any]] = {}
        self.parents: dict[str, str] = {}
        self.width = 0
        self.height = 0
        self.captures: list[dict[str, Any]] = []
        self.checks: list[dict[str, Any]] = []

    def find_scene(self) -> None:
        worlds = self.mcp.tool("world_list")
        for world in worlds:
            tree = self.mcp.tool(
                "world_tree", world=world["name"], depth=12, limit=5000
            )
            for root in tree["roots"]:
                for node in descendants(root):
                    if node["name"] == "LabScreen":
                        self.world = world["name"]
                        self.refresh_tree(tree)
                        missing = [
                            name for name in REQUIRED_NAMES if name not in self.nodes
                        ]
                        if missing:
                            raise RuntimeError(f"UiLab lacks {missing}")
                        return
        raise RuntimeError("LabScreen was not found in any running world")

    def refresh_tree(self, tree: dict[str, Any] | None = None) -> None:
        if tree is None:
            tree = self.mcp.tool("world_tree", world=self.world, depth=12, limit=5000)
        self.nodes.clear()
        self.parents.clear()

        def add(node: dict[str, Any], parent: str) -> None:
            name = node["name"]
            if name in REQUIRED_NAMES or name.startswith(("ScrollRow", "Created")):
                self.nodes[name] = node
                self.parents[name] = parent
            for child in node.get("children", []):
                add(child, name)

        for root in tree["roots"]:
            add(root, "")

    def get(self, name: str) -> dict[str, Any]:
        node = self.nodes.get(name)
        if node is None:
            raise RuntimeError(f"instance {name} absent from scene tree")
        return self.mcp.tool("instance_get", world=self.world, id=node["id"])[
            "properties"
        ]

    def rect(self, name: str) -> Rect:
        properties = self.get(name)
        position = properties.get("AbsolutePosition")
        size = properties.get("AbsoluteSize")
        if not isinstance(position, dict) or not isinstance(size, dict):
            raise TypeError(f"{name} has no resolved GUI rectangle: {properties}")
        return Rect(
            float(position["X"]),
            float(position["Y"]),
            float(size["X"]),
            float(size["Y"]),
        )

    def state(self) -> dict[str, Any]:
        return status_values(str(self.get("Status").get("Text", "")))

    def field(self, key: str) -> Any:
        state = self.state()
        if key not in state:
            raise RuntimeError(
                f"Status.Text has no {key}: {self.get('Status').get('Text')!r}"
            )
        return state[key]

    def resize(self, width: int, height: int) -> None:
        command("xdotool", "windowactivate", "--sync", self.window)
        command(
            "wmctrl",
            "-ir",
            self.window,
            "-b",
            "remove,fullscreen,maximized_vert,maximized_horz",
        )
        wait_until(
            "window leaving fullscreen",
            lambda: (
                "_NET_WM_STATE_FULLSCREEN"
                not in command("xprop", "-id", self.window, "_NET_WM_STATE")
            ),
            3,
        )
        command(
            "xdotool",
            "windowsize",
            "--sync",
            self.window,
            str(width),
            str(height),
            timeout=8,
        )
        self.width, self.height = width, height
        wait_until(
            f"window {width}x{height}",
            lambda: (
                abs(window_rect(self.window).width - width) <= 2
                and abs(window_rect(self.window).height - height) <= 2
            ),
        )
        time.sleep(0.15)

    def point(self, name: str, scroll: bool = True) -> tuple[int, int]:
        for attempt in range(25):
            viewport = Rect(0, 0, self.width, self.height)
            # ScrollingFrame clips descendants even if their resolved rectangle
            # extends into another part of the window.
            scroll_parent = self.parents.get(name, "")
            wheel_owner = ""
            while scroll_parent:
                parent = self.nodes.get(scroll_parent)
                if parent and parent.get("class") == "ScrollingFrame":
                    if scroll:
                        self.point(scroll_parent)
                    if not wheel_owner:
                        wheel_owner = scroll_parent
                    viewport = viewport.intersection(self.rect(scroll_parent))
                scroll_parent = self.parents.get(scroll_parent, "")
            target = self.rect(name)
            exposed = target.intersection(viewport)
            if (
                exposed.width >= target.width * 0.6
                and exposed.height >= target.height * 0.6
            ):
                return exposed.center
            if not scroll:
                break
            if not wheel_owner:
                break
            below = target.y + target.height - (viewport.y + viewport.height)
            above = viewport.y - target.y
            direction = 5 if below > 0 else 4
            distance = below if direction == 5 else above
            notches = min(5, max(1, math.ceil(distance / 60)))
            previous = float(self.get(wheel_owner)["CanvasPosition"]["Y"])
            wheel_x, wheel_y = viewport.center
            if wheel_owner == "Content":
                # The centered nested list may be under the pointer. Its wheel
                # handler wins over Content even when Content is the target.
                wheel_x = round(viewport.x + viewport.width - 2)
            self.move(wheel_x, wheel_y)
            result = self.mcp.tool(
                "emulate_mouse_wheel", notches=-notches if direction == 5 else notches
            )
            if result.get("queued") is not True:
                raise RuntimeError(f"scroll was not queued: {result}")
            wait_until(
                f"{wheel_owner} wheel movement",
                lambda owner=wheel_owner, old=previous: (
                    abs(float(self.get(owner)["CanvasPosition"]["Y"]) - old) > 0.1
                ),
                2,
            )
        raise RuntimeError(
            f"{name} stayed outside its visible viewport after {attempt + 1} scroll steps "
            f"(target={target}, viewport={viewport}, content_scroll={self.get('Content').get('CanvasPosition')})"
        )

    def move(self, x: int, y: int) -> None:
        command("xdotool", "mousemove", "--window", self.window, str(x), str(y))

    def click(self, name: str) -> None:
        self.move(*self.point(name))
        command("xdotool", "mousedown", "1")
        time.sleep(0.06)
        command("xdotool", "mouseup", "1")

    def press(self, name: str) -> None:
        self.move(*self.point(name))
        command("xdotool", "mousedown", "1")

    def release(self) -> None:
        command("xdotool", "mouseup", "1")

    def key(self, key: str) -> None:
        command("xdotool", "windowactivate", "--sync", self.window)
        command("xdotool", "key", key)

    def capture(self, label: str) -> Path:
        bounds = window_rect(self.window)
        if bounds.width < self.width - 2 or bounds.height < self.height - 2:
            raise RuntimeError(
                f"window capture bounds {bounds} differ from requested {self.width}x{self.height}"
            )
        screen = ImageGrab.grab().size
        if (
            bounds.x < 0
            or bounds.y < 0
            or bounds.x + bounds.width > screen[0]
            or bounds.y + bounds.height > screen[1]
        ):
            raise RuntimeError(f"window {bounds} extends beyond X11 desktop {screen}")
        image = ImageGrab.grab(
            bbox=(
                round(bounds.x),
                round(bounds.y),
                round(bounds.x + bounds.width),
                round(bounds.y + bounds.height),
            )
        )
        image = image.convert("RGB")
        path = self.output / f"{label}.png"
        image.save(path)
        self.captures.append(
            {
                "label": label,
                "file": path.name,
                "width": image.width,
                "height": image.height,
                "window": [bounds.x, bounds.y, bounds.width, bounds.height],
            }
        )
        return path

    def check(self, label: str, action: Callable[[], None]) -> None:
        if time.monotonic() >= self.deadline:
            raise RuntimeError("live check exceeded its wall-clock deadline")
        print(f"check {label}", flush=True)
        try:
            action()
        except Exception as error:  # noqa: BLE001 - collect every case failure for the gallery
            self.checks.append({"name": label, "status": "failed", "error": str(error)})
            try:
                self.capture(f"failure-{len(self.checks):02d}")
            except Exception as capture_error:  # noqa: BLE001 - retain the original failure
                self.checks[-1]["capture_error"] = str(capture_error)
            print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        else:
            self.checks.append({"name": label, "status": "passed"})


def expect_field(lab: Lab, key: str, expected: Any, seconds: float = 3) -> None:
    wait_until(f"{key}={expected!r}", lambda: lab.field(key) == expected, seconds)


def check_counter(lab: Lab, name: str, key: str, times: int) -> None:
    before = int(lab.field(key))
    for index in range(times):
        lab.click(name)
        expect_field(lab, key, before + index + 1)


def check_instant_click(lab: Lab) -> None:
    before = int(lab.field("textClicks"))
    lab.move(*lab.point("ClickButton"))
    command("xdotool", "click", "1")
    expect_field(lab, "textClicks", before + 1)


def check_batched_clicks(lab: Lab) -> None:
    before = int(lab.field("textClicks"))
    x, y = lab.point("ClickButton")
    results = lab.mcp.tool_batch(
        [("emulate_click", {"x": x, "y": y}) for _ in range(2)]
    )
    if any(result.get("queued") is not True for result in results):
        raise RuntimeError(f"batched clicks did not queue: {results}")
    expect_field(lab, "textClicks", before + 2)


def check_native_wheel(lab: Lab) -> None:
    lab.point("ClickButton")
    before = float(lab.get("Content")["CanvasPosition"]["Y"])
    content = lab.rect("Content")
    lab.move(round(content.x + 20), round(content.y + min(200, content.height / 2)))
    command("xdotool", "click", "--repeat", "8", "--delay", "100", "5")
    wait_until(
        "native wheel scroll",
        lambda: float(lab.get("Content")["CanvasPosition"]["Y"]) > before,
        2,
    )


def check_hover_and_press(lab: Lab) -> None:
    before_count = int(lab.field("textClicks"))
    x, y = lab.point("ClickButton")
    lab.move(0, 0)
    time.sleep(0.08)
    before = Image.open(lab.capture("button-idle"))
    lab.move(x, y)
    time.sleep(0.08)
    hover = Image.open(lab.capture("button-hover"))
    lab.press("ClickButton")
    try:
        time.sleep(0.08)
        pressed = Image.open(lab.capture("button-pressed"))
    finally:
        lab.release()
    expect_field(lab, "textClicks", before_count + 1)
    region = lab.rect("ClickButton")
    box = (
        max(0, round(region.x)),
        max(0, round(region.y)),
        min(before.width, round(region.x + region.width)),
        min(before.height, round(region.y + region.height)),
    )
    if box[2] <= box[0] or box[3] <= box[1]:
        raise RuntimeError("ClickButton has no visible pixels")
    hover_change = (
        ImageChops.difference(before.crop(box), hover.crop(box)).convert("L").getbbox()
    )
    press_change = (
        ImageChops.difference(hover.crop(box), pressed.crop(box)).convert("L").getbbox()
    )
    if hover_change is None or press_change is None:
        raise RuntimeError("ClickButton lacks visible hover or press feedback")


def check_drag_cancel(lab: Lab) -> None:
    before = int(lab.field("textClicks"))
    lab.press("ClickButton")
    try:
        time.sleep(0.1)
        lab.move(5, 5)
        time.sleep(0.12)
    finally:
        lab.release()
    time.sleep(0.1)
    if lab.field("textClicks") != before:
        raise RuntimeError("dragging off ClickButton activated it")


def check_text(lab: Lab) -> None:
    lab.click("Entry")
    expect_field(lab, "focused", True)
    time.sleep(0.12)
    text = "AΩ漢"
    result = lab.mcp.tool("emulate_text", text=text)
    if result.get("queued") is not True:
        raise RuntimeError(f"UTF-8 text was not queued: {result}")
    wait_until("Entry.Text UTF-8", lambda: lab.get("Entry").get("Text") == text)
    lab.key("BackSpace")
    wait_until("Entry.Text edited", lambda: lab.get("Entry").get("Text") == "AΩ")
    lab.key("Return")
    expect_field(lab, "submitted", "AΩ")


def check_ordered_edit(lab: Lab) -> None:
    lab.click("Entry")
    expect_field(lab, "focused", True)
    before = str(lab.get("Entry").get("Text", ""))
    ordered = [
        ("emulate_text", {"text": "A"}),
        ("emulate_key", {"key": "Backspace"}),
        ("emulate_text", {"text": "B"}),
    ]
    results = lab.mcp.tool_batch(ordered)
    if any(result.get("queued") is not True for result in results):
        raise RuntimeError(f"ordered edit did not queue: {results}")
    wait_until(
        "ordered text A, backspace, B",
        lambda: lab.get("Entry").get("Text") == before + "B",
    )
    results = lab.mcp.tool_batch(
        [("emulate_key", {"key": "Backspace"}) for _ in range(2)]
    )
    if any(result.get("queued") is not True for result in results):
        raise RuntimeError(f"Backspaces did not queue: {results}")
    expected = before[:-1]
    try:
        wait_until(
            "two ordered Backspaces",
            lambda: lab.get("Entry").get("Text") == expected,
        )
    except RuntimeError as error:
        actual = lab.get("Entry").get("Text")
        raise RuntimeError(
            f"{error}; Entry.Text={actual!r}, expected={expected!r}, before={before!r}"
        ) from error


def check_scale(lab: Lab) -> None:
    sizes: list[float] = []
    for name, value in (("Scale075", 0.75), ("Scale100", 1.0), ("Scale150", 1.5)):
        lab.click(name)
        expect_field(lab, "scale", value)
        sizes.append(lab.rect("ScaleSample").width)
        before = int(lab.field("sampleClicks"))
        lab.click("ScaleSample")
        expect_field(lab, "sampleClicks", before + 1)
        lab.capture(f"scale-{name[5:]}")
    if not sizes[0] < sizes[1] < sizes[2]:
        raise RuntimeError(f"UIScale did not change resolved width: {sizes}")
    if sizes[0] < 8:
        raise RuntimeError("small UIScale sample is too small to hit")


def check_aspect(lab: Lab) -> None:
    rectangle = lab.rect("AspectSample")
    if rectangle.height <= 0:
        raise RuntimeError("aspect sample has no height")
    ratio = rectangle.width / rectangle.height
    if not 1.70 <= ratio <= 1.85 or rectangle.height >= 130:
        raise RuntimeError(f"aspect sample ratio {ratio:.3f}, expected 16:9")


def check_scroll(lab: Lab) -> None:
    lab.click("ScrollRow1")
    expect_field(lab, "selectedRow", 1)
    lab.click("ScrollRow12")
    expect_field(lab, "selectedRow", 12)
    list_rect = lab.rect("ScrollList")
    row_rect = lab.rect("ScrollRow12")
    if row_rect.intersection(list_rect).height < 4:
        raise RuntimeError("selected final row is clipped")


def check_drag(lab: Lab) -> None:
    before = lab.rect("DragTarget")
    starts = int(lab.field("dragStarts"))
    ends = int(lab.field("dragEnds"))
    x, y = lab.point("DragTarget")
    lab.move(x, y)
    command("xdotool", "mousedown", "1")
    try:
        for step in range(1, 5):
            lab.move(x + step * 18, y + step * 6)
            time.sleep(0.03)
    finally:
        lab.release()
    expect_field(lab, "dragStarts", starts + 1)
    expect_field(lab, "dragEnds", ends + 1)
    after = lab.rect("DragTarget")
    if abs(after.x - before.x) < 15 and abs(after.y - before.y) < 5:
        raise RuntimeError(f"UIDragDetector did not move target: {before} -> {after}")


def check_visibility(lab: Lab) -> None:
    before = bool(lab.get("VisibilityTarget").get("Visible"))
    lab.click("ToggleVisibility")
    wait_until(
        "VisibilityTarget hidden",
        lambda: lab.get("VisibilityTarget").get("Visible") is not before,
    )
    lab.click("ToggleVisibility")
    wait_until(
        "VisibilityTarget restored",
        lambda: lab.get("VisibilityTarget").get("Visible") is before,
    )


def check_lifecycle(lab: Lab) -> None:
    start = int(lab.field("created"))
    for count in range(start + 1, 9):
        lab.click("CreateItem")
        expect_field(lab, "created", count)
        lab.refresh_tree()
        actual = {name for name in lab.nodes if name.startswith("CreatedItem")}
        expected = {f"CreatedItem{index}" for index in range(1, count + 1)}
        if actual != expected:
            raise RuntimeError(
                f"item tree after create {count}: {sorted(actual)}, expected {sorted(expected)}"
            )
        if count == 1:
            lab.click("CreatedItem1")
            wait_until(
                "new item's Activated handler",
                lambda: lab.get("Feedback").get("Text") == "CreatedItem1 activated",
            )
            expect_field(lab, "event", "CreatedItem1 activated")
            lab.capture("created-item-click")
    lab.click("CreateItem")
    time.sleep(0.1)
    if lab.field("created") != 8:
        raise RuntimeError("CreateItem exceeded the eight-item cap")
    lab.refresh_tree()
    if any(name.startswith("CreatedItem9") for name in lab.nodes):
        raise RuntimeError("ninth item exists despite cap")
    for count in range(7, -1, -1):
        lab.click("DestroyItem")
        expect_field(lab, "created", count)
        lab.refresh_tree()
        actual = {name for name in lab.nodes if name.startswith("CreatedItem")}
        expected = {f"CreatedItem{index}" for index in range(1, count + 1)}
        if actual != expected:
            raise RuntimeError(
                f"item tree after delete {count}: {sorted(actual)}, expected {sorted(expected)}"
            )


def check_reset(lab: Lab) -> None:
    lab.click("Reset")
    for key, expected in (
        ("textClicks", 0),
        ("imageClicks", 0),
        ("sampleClicks", 0),
        ("created", 0),
        ("selectedRow", 0),
        ("scale", 1.0),
        ("visible", True),
    ):
        expect_field(lab, key, expected)
    for key, expected in (
        ("focused", False),
        ("submitted", ""),
        ("dragStarts", 0),
        ("dragEnds", 0),
    ):
        expect_field(lab, key, expected)
    if lab.get("Entry").get("Text") != "":
        raise RuntimeError("reset left Entry.Text nonempty")
    cursor = lab.get("Entry").get("CursorPosition")
    if isinstance(cursor, (int, float)) and cursor > 0:
        raise RuntimeError(f"reset left active text cursor at {cursor}")
    if lab.get("Selected").get("Text") != "selected: none":
        raise RuntimeError("reset did not restore selected row label")
    if lab.get("VisibilityTarget").get("Visible") is not True:
        raise RuntimeError("reset left target hidden")
    if abs(float(lab.get("ScaleControl").get("Scale", -1)) - 1.0) > 0.01:
        raise RuntimeError("reset did not restore UIScale")
    if abs(lab.rect("ScaleSample").width - 164) > 2:
        raise RuntimeError("reset did not restore sample width")
    canvas = lab.get("ScrollList").get("CanvasPosition", {})
    if abs(float(canvas.get("Y", -1))) > 1:
        raise RuntimeError(f"reset left ScrollList at {canvas}")
    area = lab.rect("DragArea")
    target = lab.rect("DragTarget")
    if abs(target.x - area.x - 12) > 2 or abs(target.y - area.y - 19) > 2:
        raise RuntimeError(f"reset left DragTarget at {target} inside {area}")
    lab.refresh_tree()
    if any(name.startswith("CreatedItem") for name in lab.nodes):
        raise RuntimeError("reset left created items in the instance tree")


def write_report(
    lab: Lab, output: Path, preset: str, client: Path, elapsed: float, error: str | None
) -> None:
    report = {
        "suite": "ui-live-check",
        "preset": preset,
        "client": str(client),
        "elapsed_seconds": round(elapsed, 2),
        "checks": lab.checks,
        "captures": lab.captures,
        "fatal_error": error,
        "passed": error is None
        and bool(lab.checks)
        and all(row["status"] == "passed" for row in lab.checks),
    }
    (output / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    figures = "\n".join(
        f'<figure><a href="{html.escape(row["file"])}"><img loading="lazy" src="{html.escape(row["file"])}" '
        f'alt="{html.escape(row["label"])}"></a><figcaption>{html.escape(row["label"])} '
        f"{row['width']}x{row['height']}</figcaption></figure>"
        for row in lab.captures
    )
    checks = "\n".join(
        f'<li class="{row["status"]}">{html.escape(row["name"])}: '
        f"{html.escape(row.get('error', row['status']))}</li>"
        for row in lab.checks
    )
    page = f"""<!doctype html><html lang="en"><meta charset="utf-8"><title>UiLab live check</title>
<style>html{{background:#000;color:#fff;font:14px sans-serif}}body{{margin:24px}}a{{color:#fff}}figure{{margin:0 0 28px}}
img{{max-width:100%;max-height:720px;border:1px solid #333}}figcaption{{margin-top:6px}}.failed{{color:#f88}}
ul{{padding-left:20px}}main{{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:20px}}</style>
<h1>UiLab live check</h1><p>{"PASS" if report["passed"] else "FAIL"} | {html.escape(preset)} | {elapsed:.1f}s</p>
<ul>{checks}</ul><main>{figures}</main></html>"""
    (output / "report.html").write_text(page, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", default="dev")
    parser.add_argument("--client", type=Path)
    parser.add_argument("--scene", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--seconds", type=int, default=180, help="wall-clock failsafe")
    parser.add_argument(
        "--sizes", nargs="*", default=[f"{w}x{h}" for w, h in DEFAULT_SIZES]
    )
    return parser.parse_args()


def run() -> int:
    arguments = parse_args()
    client = (
        arguments.client
        or ROOT / ".cache" / "build" / arguments.preset / "client" / "client"
    ).resolve()
    scene = (
        arguments.scene
        or ROOT
        / ".cache"
        / "build"
        / arguments.preset
        / "assets"
        / "examples"
        / "scripts"
        / "UiLab.luau"
    ).resolve()
    output = (
        arguments.out or ROOT / ".cache" / "build" / arguments.preset / "ui-live-check"
    ).resolve()
    output.mkdir(parents=True, exist_ok=True)
    clear_previous_report(output)
    sizes: list[tuple[int, int]] = []
    try:
        for item in arguments.sizes:
            if not re.fullmatch(r"\d+x\d+", item):
                raise ValueError(f"bad size {item!r}; use WIDTHxHEIGHT")
            sizes.append(tuple(map(int, item.split("x"))))
        if not sizes:
            raise ValueError("at least one --sizes entry is required")
        for executable in ("xdotool", "xwininfo", "xrandr", "xprop", "wmctrl"):
            if shutil.which(executable) is None:
                raise RuntimeError(f"{executable} is required")
        if not os.environ.get("DISPLAY"):
            raise RuntimeError("DISPLAY is required for the native window check")
        if not client.is_file() or not os.access(client, os.X_OK):
            raise RuntimeError(f"client binary missing: {client}")
        if not scene.is_file():
            raise RuntimeError(
                f"staged UiLab scene missing: {scene}; build the client first"
            )
        origin = display_origin(sizes)
    except (OSError, ValueError, RuntimeError) as error:
        failure = {
            "suite": "ui-live-check",
            "checks": [],
            "captures": [],
            "fatal_error": str(error),
            "passed": False,
        }
        (output / "report.json").write_text(
            json.dumps(failure, indent=2) + "\n", encoding="utf-8"
        )
        (output / "report.html").write_text(
            f'<!doctype html><html><meta charset="utf-8"><title>UiLab live check failed</title>'
            f'<body style="background:#000;color:#fff"><h1>UiLab live check failed</h1><p>{html.escape(str(error))}</p></body></html>',
            encoding="utf-8",
        )
        print(f"FATAL: {error}", file=sys.stderr)
        return 1
    started = time.monotonic()
    port = free_port()
    log = (output / "client.log").open("w", encoding="utf-8")
    process: subprocess.Popen[str] | None = None
    mcp: Mcp | None = None
    lab: Lab | None = None
    fatal: str | None = None
    try:
        process = subprocess.Popen(
            [
                str(client),
                "--script",
                str(scene),
                "--mcp-port",
                str(port),
                "--width",
                "1280",
                "--height",
                "720",
                "--uncapped",
                "--max-fps",
                "60",
                "--frames",
                "1000000000",
                "--profile-snapshot",
                str(output / "frame-profile.txt"),
            ],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )

        def connect() -> Mcp | None:
            if process.poll() is not None:
                raise RuntimeError(
                    f"client exited early with {process.returncode}; see {output / 'client.log'}"
                )
            try:
                return Mcp(port)
            except OSError:
                return None

        mcp = wait_until("client MCP listener", connect, 20)
        window = wait_until(
            "child client window", lambda: child_window(process.pid), 20
        )
        command("xdotool", "windowactivate", "--sync", window)
        place_window(window, origin)
        lab = Lab(mcp, window, output, started + arguments.seconds)
        wait_until("LabScreen scene", lambda: _try_scene(lab), 15)
        for width, height in sizes:
            label = f"{width}x{height}"

            def baseline(
                width: int = width, height: int = height, label: str = label
            ) -> None:
                lab.resize(width, height)
                lab.capture(f"baseline-{label}")
                screen = lab.rect("Backdrop")
                if screen.width < width - 4 or screen.height < height - 4:
                    raise RuntimeError(f"LabScreen covers only {screen} at {label}")
                before = int(lab.field("textClicks"))
                lab.click("ClickButton")
                expect_field(lab, "textClicks", before + 1)
                lab.capture(f"button-{label}")
                lab.click("Reset")
                expect_field(lab, "textClicks", 0)

            lab.check(f"resize and click {label}", baseline)
        lab.resize(1280, 720)
        lab.check("reset before interaction suite", lambda: check_reset(lab))
        lab.check("instant native click", lambda: check_instant_click(lab))
        lab.check("batched control clicks", lambda: check_batched_clicks(lab))
        lab.check("native wheel scroll", lambda: check_native_wheel(lab))
        lab.check("hover and pressed pixels", lambda: check_hover_and_press(lab))
        lab.check(
            "repeated text clicks",
            lambda: check_counter(lab, "ClickButton", "textClicks", 3),
        )
        lab.check("drag off cancels click", lambda: check_drag_cancel(lab))
        lab.check(
            "image button", lambda: check_counter(lab, "ImageButton", "imageClicks", 2)
        )
        lab.check("UTF-8 edit and submit", lambda: check_text(lab))
        lab.check("ordered text and key input", lambda: check_ordered_edit(lab))
        lab.check("UIScale sizes and hit target", lambda: check_scale(lab))
        lab.check("aspect ratio", lambda: check_aspect(lab))
        lab.check("scroll and clipped row selection", lambda: check_scroll(lab))
        lab.check("UIDragDetector motion", lambda: check_drag(lab))
        lab.check("visibility toggles", lambda: check_visibility(lab))
        lab.check("bounded create and destroy", lambda: check_lifecycle(lab))
        lab.check("reset restores state", lambda: check_reset(lab))
        lab.capture("final-before-profile")
        mcp.tool("profile_frame")
        profile = wait_until("live profile spans", lambda: _profile_spans(mcp), 3)
        (output / "profile-live.json").write_text(
            json.dumps(profile, indent=2) + "\n", encoding="utf-8"
        )
    except Exception as error:  # noqa: BLE001 - always stop the child and write a report
        fatal = str(error)
        print(f"FATAL: {fatal}", file=sys.stderr, flush=True)
        if lab is not None:
            try:
                lab.capture("fatal")
            except Exception as capture_error:  # noqa: BLE001 - preserve the startup failure
                fatal = f"{fatal}; fatal capture failed: {capture_error}"
    finally:
        if process is not None:
            if process.poll() is None and lab is not None:
                try:
                    lab.key("F12")
                    process.wait(timeout=4)
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                    pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=4)
            if process.returncode not in (0, None) and fatal is None:
                fatal = f"client exited with {process.returncode}; see {output / 'client.log'}"
        if mcp is not None:
            mcp.close()
        log.close()
    if lab is None:
        lab = Lab.__new__(Lab)
        lab.checks = []
        lab.captures = []
    if fatal is None:
        try:
            with (output / "frame-profile.txt").open(encoding="utf-8") as snapshot:
                if snapshot.readline().strip() != "atomic frame graph snapshot":
                    raise ValueError("frame-profile.txt has no frame graph header")
            live_profile = json.loads(
                (output / "profile-live.json").read_text(encoding="utf-8")
            )
            if (
                not isinstance(live_profile, dict)
                or not isinstance(live_profile.get("spans"), list)
                or not live_profile["spans"]
            ):
                raise ValueError("profile-live.json has no spans")
        except (OSError, UnicodeError, ValueError) as error:
            fatal = f"profile evidence invalid: {error}"
    write_report(
        lab, output, arguments.preset, client, time.monotonic() - started, fatal
    )
    print(
        f"{'PASS' if fatal is None and all(row['status'] == 'passed' for row in lab.checks) else 'FAIL'}: {output / 'report.html'}"
    )
    return (
        0
        if fatal is None
        and bool(lab.checks)
        and all(row["status"] == "passed" for row in lab.checks)
        else 1
    )


def _try_scene(lab: Lab) -> bool:
    try:
        lab.find_scene()
    except RuntimeError:
        return False
    return True


def _profile_spans(mcp: Mcp) -> dict[str, Any] | None:
    profile = mcp.tool("profile_frame")
    return profile if profile.get("spans") else None


if __name__ == "__main__":
    try:
        sys.exit(run())
    except (OSError, ValueError, RuntimeError) as error:
        sys.exit(f"ui-live-check: {error}")
