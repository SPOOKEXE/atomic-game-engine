#!/usr/bin/env python3
"""Renders a folded-stack file as a self-contained SVG flamegraph.

**Here rather than in a C++ tool because its input is a text format and its
output is a picture.** `mono.tools/AGENTS.md` says a tool is C++ when it is a
program and CMake when its input is CMake's own output; this is neither. It
reads what `core::FrameGraph::WriteFolded` wrote and draws it, and nothing in
the engine links it or depends on it existing.

**Stdlib only.** `perf` cannot record on this machine - `perf_event_paranoid` is
4 - so the profile comes from the engine's own frame graph and the renderer has
to run wherever the engine builds, with no pip install in front of it.

The input is one line per stack:

    root;child;leaf 12345

where the number is the microseconds spent in that stack and not in a deeper
one. A frame's width is its own line plus every line beneath it, which is what
makes the format additive: two captures of the same run may be concatenated.

Usage:

    scripts/flamegraph.py in.folded --svg out.svg --top out_top.txt

Multiprocess captures keep main at the requested path and write child captures
beside it as `<label>.processN.folded`. Select one without spelling its derived
path, or list the captures that exist:

    scripts/flamegraph.py run.folded --worker process2 --svg process2.svg
    scripts/flamegraph.py run.folded --list-workers

The colour of a frame is a hash of its name, so the same span is the same
colour in two captures and a diff by eye is possible at all.

Averaging mode reads several `<label>.window<TICKS>.folded` snapshots -
written periodically by `server.profile-window` alongside the usual whole-run
capture - and draws each frame's *mean* self time per frame across them,
instead of one run's raw total:

    scripts/flamegraph.py --average out.window*.folded --svg avg.svg --top avg_top.txt

`--top` in this mode also lists each frame's min/max/avg/median self time per
frame, which a single capture cannot show: it is one number, with no
distribution behind it to describe.
"""

from __future__ import annotations

import argparse
import html
import re
import statistics
import sys
from pathlib import Path

# Enough rows for the frame graph's own depth budget - `FrameGraph::
# MAXIMUM_DEPTH` is 12 - with room for a title and the frames a deeper capture
# would carry.
ROW_HEIGHT = 17
FONT_SIZE = 11
IMAGE_WIDTH = 1600
LEFT_MARGIN = 8
TOP_MARGIN = 46
BOTTOM_MARGIN = 12

# Below this a rectangle cannot hold a legible label, so it gets none. It still
# gets its hover title, which is what a reader zooms in with.
MINIMUM_LABEL_WIDTH = 26.0

# Under a pixel wide is a rectangle nobody can point at and a line of SVG that
# costs as much as one they can. A stress capture has thousands of them.
MINIMUM_DRAWN_WIDTH = 0.08


class Node:
	"""One frame in the merged call tree."""

	__slots__ = ("Name", "Children", "SelfMicroseconds", "TotalMicroseconds")

	def __init__(self, name: str) -> None:
		self.Name = name
		self.Children: dict[str, Node] = {}
		self.SelfMicroseconds = 0.0
		self.TotalMicroseconds = 0.0

	def Child(self, name: str) -> "Node":
		found = self.Children.get(name)
		if found is None:
			found = Node(name)
			self.Children[name] = found
		return found


def ReadFlat(path: Path) -> dict[str, float]:
	"""Reads a folded file into stack -> self-microseconds, with no tree built.

	A malformed line is skipped rather than fatal: a capture truncated by a
	killed process is still worth drawing, and refusing it would lose the run.
	A stack seen twice - one capture concatenated onto another - adds, which is
	the same addition `AccumulateFoldedStacks` does in the engine.
	"""
	flat: dict[str, float] = {}
	for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
		line = line.strip()
		if not line or line.startswith("#"):
			continue
		cut = line.rfind(" ")
		if cut < 0:
			continue
		try:
			value = float(line[cut + 1 :])
		except ValueError:
			continue
		stack = line[:cut]
		flat[stack] = flat.get(stack, 0.0) + value
	return flat


def BuildTree(flat: dict[str, float]) -> Node:
	"""Builds the merged call tree a flat stack -> self-microseconds map describes."""
	root = Node("all")
	for stack, value in flat.items():
		node = root
		for frame in stack.split(";"):
			if frame:
				node = node.Child(frame)
		node.SelfMicroseconds += value
	return root


def ReadFolded(path: Path) -> Node:
	"""Builds the tree a folded file describes."""
	return BuildTree(ReadFlat(path))


def Total(node: Node) -> float:
	"""Fills in inclusive time, bottom up, and returns this node's."""
	node.TotalMicroseconds = node.SelfMicroseconds + sum(Total(child) for child in node.Children.values())
	return node.TotalMicroseconds


def Colour(name: str) -> str:
	"""A stable warm colour per frame name.

	Stable is the whole requirement: comparing a baseline with an iteration means
	looking for a block that changed size, and a palette assigned by position
	would repaint the graph every time a span moved.
	"""
	digest = 0
	for letter in name:
		digest = (digest * 131 + ord(letter)) & 0xFFFFFFFF
	red = 190 + (digest & 0x3F)
	green = 80 + ((digest >> 6) & 0x5F)
	blue = 40 + ((digest >> 12) & 0x2F)
	return f"#{red:02x}{green:02x}{blue:02x}"


def Draw(node: Node, x: float, depth: int, scale: float, height: float, out: list[str], grand: float) -> None:
	"""Emits one rectangle per node, root at the bottom."""
	width = node.TotalMicroseconds * scale
	if width < MINIMUM_DRAWN_WIDTH:
		return

	y = height - BOTTOM_MARGIN - (depth + 1) * ROW_HEIGHT
	share = 100.0 * node.TotalMicroseconds / grand if grand > 0 else 0.0
	label = html.escape(node.Name)
	title = (
		f"{label} - {node.TotalMicroseconds / 1000.0:.3f} ms total, "
		f"{node.SelfMicroseconds / 1000.0:.3f} ms self, {share:.2f}%"
	)

	out.append(
		f'<g><title>{title}</title>'
		f'<rect x="{x:.2f}" y="{y:.1f}" width="{max(width - 0.6, 0.2):.2f}" height="{ROW_HEIGHT - 1}" '
		f'fill="{Colour(node.Name)}" rx="1"/>'
	)
	if width > MINIMUM_LABEL_WIDTH:
		# Roughly the characters that fit at this font size. Measuring properly
		# needs the font, which a self-contained SVG does not have.
		room = int((width - 6) / (FONT_SIZE * 0.55))
		text = node.Name if len(node.Name) <= room else node.Name[: max(room - 1, 1)] + "…"
		out.append(
			f'<text x="{x + 3:.2f}" y="{y + ROW_HEIGHT - 5}" font-size="{FONT_SIZE}" '
			f'font-family="monospace" fill="#000">{html.escape(text)}</text>'
		)
	out.append("</g>")

	cursor = x
	# Widest first, so the shape of two captures is comparable left to right
	# rather than depending on which name sorted first.
	for child in sorted(node.Children.values(), key=lambda entry: -entry.TotalMicroseconds):
		Draw(child, cursor, depth + 1, scale, height, out, grand)
		cursor += child.TotalMicroseconds * scale


def Depth(node: Node) -> int:
	return 1 + max((Depth(child) for child in node.Children.values()), default=0)


def WriteSvg(root: Node, path: Path, title: str) -> None:
	grand = root.TotalMicroseconds
	if grand <= 0:
		raise SystemExit(f"{path}: the capture has no time in it")

	rows = Depth(root)
	height = TOP_MARGIN + rows * ROW_HEIGHT + BOTTOM_MARGIN
	scale = (IMAGE_WIDTH - 2 * LEFT_MARGIN) / grand

	out: list[str] = []
	out.append(
		f'<svg xmlns="http://www.w3.org/2000/svg" width="{IMAGE_WIDTH}" height="{height:.0f}" '
		f'viewBox="0 0 {IMAGE_WIDTH} {height:.0f}">'
	)
	out.append(f'<rect width="100%" height="100%" fill="#000"/>')
	out.append(
		f'<text x="{LEFT_MARGIN}" y="22" font-size="15" font-family="monospace" fill="#fff">'
		f"{html.escape(title)}</text>"
	)
	out.append(
		f'<text x="{LEFT_MARGIN}" y="38" font-size="11" font-family="monospace" fill="#888">'
		f"{grand / 1000.0:.1f} ms of measured self time across {len(root.Children)} root span(s)"
		f"</text>"
	)

	cursor = float(LEFT_MARGIN)
	for child in sorted(root.Children.values(), key=lambda entry: -entry.TotalMicroseconds):
		Draw(child, cursor, 0, scale, height, out, grand)
		cursor += child.TotalMicroseconds * scale

	out.append("</svg>\n")
	path.write_text("".join(out), encoding="utf-8")


def WriteTop(root: Node, path: Path, count: int, title: str) -> None:
	"""The greppable half, so two iterations diff as text and not only by eye."""
	bySelf: dict[str, float] = {}
	byTotal: dict[str, float] = {}
	stacks: list[tuple[float, str]] = []

	def Walk(node: Node, stack: str) -> None:
		for child in node.Children.values():
			path_ = f"{stack};{child.Name}" if stack else child.Name
			bySelf[child.Name] = bySelf.get(child.Name, 0.0) + child.SelfMicroseconds
			byTotal[child.Name] = byTotal.get(child.Name, 0.0) + child.TotalMicroseconds
			stacks.append((child.SelfMicroseconds, path_))
			Walk(child, path_)

	Walk(root, "")
	grand = root.TotalMicroseconds

	lines = [title, f"total measured self time  {grand / 1000.0:.3f} ms", ""]

	lines.append(f"top {count} frames by self time (merged over every stack)")
	lines.append(f"{'frame':<48} {'ms':>12} {'share':>8}")
	for name, value in sorted(bySelf.items(), key=lambda entry: -entry[1])[:count]:
		lines.append(f"{name:<48} {value / 1000.0:>12.3f} {100.0 * value / grand:>7.2f}%")

	lines.append("")
	lines.append(f"top {count} frames by total time (merged over every stack)")
	lines.append(f"{'frame':<48} {'ms':>12} {'share':>8}")
	for name, value in sorted(byTotal.items(), key=lambda entry: -entry[1])[:count]:
		lines.append(f"{name:<48} {value / 1000.0:>12.3f} {100.0 * value / grand:>7.2f}%")

	lines.append("")
	lines.append(f"top {count} stacks by self time")
	for value, stack in sorted(stacks, key=lambda entry: -entry[0])[:count]:
		lines.append(f"{value / 1000.0:>12.3f} ms  {stack}")

	path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# Matches what `server.profile-window` names a snapshot - the tick count it was
# written at, so two snapshots subtract into the window between them with no
# window tracking on the server's side.
WINDOW_PATTERN = re.compile(r"\.window(\d+)\.folded$")
WORKER_PATTERN = re.compile(r"^(?:process)?(\d+)$")


def WindowTicks(path: Path) -> int:
	"""The absolute tick a window snapshot was written at, from its filename."""
	match = WINDOW_PATTERN.search(path.name)
	if not match:
		raise SystemExit(f"{path}: not a '<label>.window<TICKS>.folded' snapshot")
	return int(match.group(1))


def WorkerName(value: str) -> str:
	"""Normalises a selector to `main` or `processN`."""
	worker = value.lower().replace(" ", "")
	if worker == "main":
		return worker
	match = WORKER_PATTERN.fullmatch(worker)
	if not match or int(match.group(1)) <= 0:
		raise argparse.ArgumentTypeError("worker must be main, N, or processN")
	return f"process{int(match.group(1))}"


def SelectWorker(path: Path, worker: str) -> Path:
	"""Derives one process capture from the main capture's path."""
	if worker == "main":
		return path
	stem = path.stem
	window = re.search(r"\.window\d+$", stem)
	if window:
		stem = stem[: window.start()] + f".{worker}" + stem[window.start() :]
	else:
		stem += f".{worker}"
	return path.with_name(stem + path.suffix)


def AvailableWorkers(path: Path) -> list[str]:
	"""Lists selectors with a whole-run or window capture beside `path`."""
	stem = re.sub(r"\.window\d+$", "", path.stem)
	workers = {"main"} if path.is_file() else set()
	if not path.parent.is_dir():
		return sorted(workers)
	pattern = re.compile(rf"^{re.escape(stem)}\.process(\d+)(?:\.window\d+)?{re.escape(path.suffix)}$")
	for candidate in path.parent.iterdir():
		match = pattern.fullmatch(candidate.name)
		if match:
			workers.add(f"process{int(match.group(1))}")
	return sorted(workers, key=lambda name: 0 if name == "main" else int(name.removeprefix("process")))


def Mean(values: list[float]) -> float:
	return sum(values) / len(values) if values else 0.0


def PerFrameAverages(paths: list[Path]) -> tuple[dict[str, list[float]], list[int]]:
	"""Every stack's self time *per frame*, one reading per window, in microseconds.

	Each snapshot is cumulative since folding turned on - `FrameGraph::
	WriteFolded` reads the running total without resetting it - so two
	snapshots N ticks apart subtract into exactly that window's folded stacks.
	Dividing by the tick count between them turns a window's total into a
	per-frame figure, which is what makes windows of different sizes (the last
	one is usually short) comparable and averageable at all.

	A stack absent from a window reads zero for it rather than being left out:
	it really did cost nothing that window, and skipping it would bias the
	average toward the windows it happened to appear in.

	@return (stack -> one reading per window, oldest first), and the tick
	        count of each window in the same order.
	"""
	ordered = sorted(paths, key=WindowTicks)
	previousTicks = 0
	previousFlat: dict[str, float] = {}
	perWindow: list[dict[str, float]] = []
	windowSizes: list[int] = []
	for path in ordered:
		ticks = WindowTicks(path)
		frames = ticks - previousTicks
		if frames <= 0:
			raise SystemExit(f"{path}: window ticks did not advance past {previousTicks}")
		current = ReadFlat(path)
		stacks = current.keys() | previousFlat.keys()
		perWindow.append(
			{stack: (current.get(stack, 0.0) - previousFlat.get(stack, 0.0)) / frames for stack in stacks}
		)
		windowSizes.append(frames)
		previousTicks, previousFlat = ticks, current

	readings: dict[str, list[float]] = {}
	for index, window in enumerate(perWindow):
		for stack, value in window.items():
			readings.setdefault(stack, [0.0] * len(perWindow))[index] = value
	return readings, windowSizes


def WriteAverageTop(
	readings: dict[str, list[float]], windowSizes: list[int], path: Path, count: int, title: str
) -> None:
	"""The averaging mode's greppable half: min/max/avg/median self time per frame.

	Merged by frame *name* rather than by full stack, the same cut `WriteTop`
	makes for `bySelf` - a leaf like `Authority::Score` reached through more
	than one parent should read as one row, not split across its callers.
	"""
	byName: dict[str, list[float]] = {}
	for stack, values in readings.items():
		name = stack.rsplit(";", 1)[-1]
		series = byName.setdefault(name, [0.0] * len(windowSizes))
		for index, value in enumerate(values):
			series[index] += value

	grand = sum(Mean(series) for series in byName.values())
	sizes = sorted(set(windowSizes))
	sizeText = f"{sizes[0]} tick(s)" if len(sizes) == 1 else f"{sizes[0]}-{sizes[-1]} tick(s)"

	lines = [
		title,
		f"{len(windowSizes)} window(s) of {sizeText} each - readings are self time per frame",
		f"average self time per frame across every window  {grand / 1000.0:.3f} ms",
		"",
	]

	lines.append(f"top {count} frames by average self time per frame")
	lines.append(f"{'frame':<48} {'avg ms':>10} {'min ms':>10} {'max ms':>10} {'median ms':>10} {'share':>8}")
	for name, series in sorted(byName.items(), key=lambda entry: -Mean(entry[1]))[:count]:
		avg = Mean(series)
		share = 100.0 * avg / grand if grand > 0 else 0.0
		lines.append(
			f"{name:<48} {avg / 1000.0:>10.4f} {min(series) / 1000.0:>10.4f} "
			f"{max(series) / 1000.0:>10.4f} {statistics.median(series) / 1000.0:>10.4f} {share:>7.2f}%"
		)

	path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
	parser = argparse.ArgumentParser(description="Render a folded-stack capture as an SVG flamegraph.")
	parser.add_argument("folded", type=Path, nargs="?", help="the .folded file to read")
	parser.add_argument(
		"--average",
		type=Path,
		nargs="+",
		metavar="WINDOW",
		help=(
			"average mode: one or more '<label>.window<TICKS>.folded' snapshots written by "
			"server.profile-window, in place of FOLDED. The bar is each frame's mean self time "
			"per frame across the windows; --top also gets a min/max/avg/median table."
		),
	)
	parser.add_argument("--svg", type=Path, help="where to write the graph (default: alongside, .svg)")
	parser.add_argument("--top", type=Path, help="where to write the text summary")
	parser.add_argument("--count", type=int, default=20, help="how many rows the summary lists")
	parser.add_argument("--title", default="", help="the heading drawn on the graph")
	parser.add_argument(
		"--worker", type=WorkerName, default="main", help="capture to read: main, N, or processN"
	)
	parser.add_argument("--list-workers", action="store_true", help="list available process captures and exit")
	arguments = parser.parse_args()

	if arguments.list_workers:
		if arguments.folded is None:
			print("FOLDED is required with --list-workers", file=sys.stderr)
			return 2
		for worker in AvailableWorkers(arguments.folded):
			print(worker)
		return 0

	if arguments.average:
		arguments.average = [SelectWorker(path, arguments.worker) for path in arguments.average]
		missing = [path for path in arguments.average if not path.is_file()]
		if missing:
			print(f"no such window snapshot(s): {', '.join(str(path) for path in missing)}", file=sys.stderr)
			return 2

		readings, windowSizes = PerFrameAverages(arguments.average)
		root = BuildTree({stack: Mean(values) for stack, values in readings.items()})
		Total(root)

		title = arguments.title or arguments.average[0].name.split(".window")[0]
		svg = arguments.svg or arguments.average[0].with_suffix(".svg")
		WriteSvg(root, svg, f"{title} - {len(windowSizes)}-window average")
		print(f"wrote {svg}")

		if arguments.top:
			WriteAverageTop(readings, windowSizes, arguments.top, arguments.count, title)
			print(f"wrote {arguments.top}")

		return 0

	if arguments.folded is None:
		print("either FOLDED or --average is required", file=sys.stderr)
		return 2
	arguments.folded = SelectWorker(arguments.folded, arguments.worker)
	if not arguments.folded.is_file():
		print(f"no such capture: {arguments.folded}", file=sys.stderr)
		return 2

	root = ReadFolded(arguments.folded)
	Total(root)

	title = arguments.title or arguments.folded.stem
	svg = arguments.svg or arguments.folded.with_suffix(".svg")
	WriteSvg(root, svg, title)
	print(f"wrote {svg}")

	if arguments.top:
		WriteTop(root, arguments.top, arguments.count, title)
		print(f"wrote {arguments.top}")

	return 0


if __name__ == "__main__":
	raise SystemExit(main())
