#!/usr/bin/env python3
"""Run every staged top-level demo and validate its BMP capture."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
CAPTURE_SIZE = (640, 360)
ORDINARY_TIMEOUT_SECONDS = 45
STRESS_TIMEOUT_SECONDS = 600
STRESS_DEMO = "PbrTextureUniqueStress.luau"
LONG_TIMEOUT_DEMOS = {STRESS_DEMO, "StressPhysics.luau"}
LOG_FAILURE_MARKERS = ("[error]", "[critical]", "heartbeat:")


def positive_int(value: str) -> int:
	try:
		parsed = int(value)
	except ValueError as error:
		raise argparse.ArgumentTypeError("must be a positive integer") from error
	if parsed < 1:
		raise argparse.ArgumentTypeError("must be a positive integer")
	return parsed


def staged_demos(build: Path) -> list[tuple[str, Path]]:
	items: list[tuple[str, Path]] = []
	items.extend(("script", path) for path in sorted((build / "assets/examples/scripts").glob("*.luau")))
	items.extend(("script", path) for path in sorted((build / "assets/examples/scripts").glob("*.js")))
	items.extend(("world", path) for path in sorted((build / "assets/examples/worlds").glob("*.aworld")))
	return items


def bmp_size(path: Path) -> tuple[int, int]:
	with path.open("rb") as image:
		header = image.read(54)
		actual_size = path.stat().st_size
	if len(header) < 54 or header[:2] != b"BM":
		raise ValueError("missing BMP file header")
	declared_size = struct.unpack_from("<I", header, 2)[0]
	pixel_offset = struct.unpack_from("<I", header, 10)[0]
	dib_size = struct.unpack_from("<I", header, 14)[0]
	width, height = struct.unpack_from("<ii", header, 18)
	plane_count = struct.unpack_from("<H", header, 26)[0]
	bits_per_pixel = struct.unpack_from("<H", header, 28)[0]
	compression = struct.unpack_from("<I", header, 30)[0]
	image_size = struct.unpack_from("<I", header, 34)[0]
	if dib_size < 40 or pixel_offset < 14 + dib_size or pixel_offset > actual_size:
		raise ValueError("invalid BMP DIB or pixel offset")
	if plane_count != 1 or declared_size not in (0, actual_size):
		raise ValueError("invalid BMP plane count or file size")
	if width <= 0 or height == 0 or bits_per_pixel not in (24, 32) or compression not in (0, 3):
		raise ValueError("unsupported BMP dimensions, pixel format, or compression")
	row_bytes = ((width * bits_per_pixel + 31) // 32) * 4
	pixel_bytes = row_bytes * abs(height)
	if pixel_offset + pixel_bytes > actual_size or image_size not in (0, pixel_bytes):
		raise ValueError("truncated BMP pixel data")
	return width, abs(height)


def write_results(path: Path, results: list[dict[str, object]]) -> None:
	temporary = path.with_suffix(".tmp")
	temporary.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
	temporary.replace(path)


def failure_lines(path: Path) -> list[str]:
	markers = tuple(marker.casefold() for marker in LOG_FAILURE_MARKERS)
	with path.open(encoding="utf-8", errors="replace") as log:
		return [line.rstrip() for line in log if any(marker in line.casefold() for marker in markers)]


def command_for(client: Path, kind: str, demo: Path, capture: Path, frames: int) -> list[str]:
	command = [
		str(client),
		"--headless",
		"--entities",
		"0",
		"--frames",
		str(frames),
		"--uncapped",
		"--max-fps",
		"60",
		"--width",
		str(CAPTURE_SIZE[0]),
		"--height",
		str(CAPTURE_SIZE[1]),
		"--capture",
		str(capture),
	]
	if demo.name == STRESS_DEMO:
		command.extend(("--texture-budget-mib", "9216"))
	if demo.name.startswith("DataFactory"):
		command.append("--data-factory")
	command.extend(("--script" if kind == "script" else "--game", str(demo)))
	return command


def run_demo(
	client: Path,
	run_dir: Path,
	kind: str,
	demo: Path,
	frames: int,
	ordinary_timeout: int,
	stress_timeout: int,
	index: int,
	count: int,
) -> dict[str, object]:
	item_dir = run_dir / kind / demo.name
	item_dir.mkdir(parents=True, exist_ok=True)
	capture = item_dir / "capture.bmp"
	log_path = item_dir / "client.log"
	timeout_seconds = stress_timeout if demo.name in LONG_TIMEOUT_DEMOS else ordinary_timeout
	command = command_for(client, kind, demo, capture, frames)
	environment = {**os.environ, "SDL_GPU_DRIVER": "vulkan", "SDL_AUDIODRIVER": "dummy"}
	started = time.monotonic()
	print(f"[{index}/{count}] running {demo.name} (timeout {timeout_seconds}s)", flush=True)

	return_code = -1
	timed_out = False
	launch_error = ""
	try:
		with log_path.open("w", encoding="utf-8", errors="replace") as log:
			process = subprocess.Popen(
				command,
				cwd=ROOT,
				env=environment,
				stdout=log,
				stderr=subprocess.STDOUT,
				text=True,
			)
			deadline = started + timeout_seconds
			next_status = started + 30
			while process.poll() is None:
				now = time.monotonic()
				if now >= deadline:
					process.kill()
					process.wait()
					timed_out = True
					break
				if now >= next_status:
					elapsed = int(now - started)
					print(f"[{index}/{count}] {demo.name} still running ({elapsed}s)", flush=True)
					next_status = now + 30
				time.sleep(0.25)
			return_code = process.returncode
	except OSError as error:
		launch_error = str(error)

	elapsed = round(time.monotonic() - started, 3)
	issues = failure_lines(log_path) if log_path.exists() else []
	if launch_error:
		issues.append(f"could not start client: {launch_error}")
	if timed_out:
		issues.append(f"timed out after {timeout_seconds} seconds")

	capture_width = 0
	capture_height = 0
	if capture.is_file():
		try:
			capture_width, capture_height = bmp_size(capture)
		except (OSError, ValueError, struct.error) as error:
			issues.append(f"invalid capture: {error}")
	else:
		issues.append("capture was not written")
	if (capture_width, capture_height) != CAPTURE_SIZE:
		issues.append(f"capture is {capture_width}x{capture_height}, expected {CAPTURE_SIZE[0]}x{CAPTURE_SIZE[1]}")
	if return_code != 0:
		issues.append(f"client exited with status {return_code}")

	result: dict[str, object] = {
		"name": demo.name,
		"kind": kind,
		"exit_status": return_code,
		"timed_out": timed_out,
		"seconds": elapsed,
		"capture": str(capture.relative_to(run_dir)) if capture.is_file() else None,
		"capture_width": capture_width,
		"capture_height": capture_height,
		"log": str(log_path.relative_to(run_dir)),
		"issues": issues,
		"passed": not issues,
	}
	if issues:
		print(f"[{index}/{count}] FAIL {demo.name}: {'; '.join(issues)}", flush=True)
	else:
		print(f"[{index}/{count}] PASS {demo.name} {capture_width}x{capture_height} {elapsed}s", flush=True)
	return result


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build", default=".cache/build/dev", help="selected build directory")
	parser.add_argument("--frames", type=positive_int, default=120, help="frames to render per demo")
	parser.add_argument("--timeout", type=positive_int, default=ORDINARY_TIMEOUT_SECONDS, help="seconds per ordinary demo")
	parser.add_argument(
		"--stress-timeout", type=positive_int, default=STRESS_TIMEOUT_SECONDS, help="seconds for long stress demos"
	)
	parser.add_argument("--only", help="regular expression selecting demo filenames")
	args = parser.parse_args()

	build = Path(args.build)
	if not build.is_absolute():
		build = ROOT / build
	build = build.resolve()
	client = build / "client/client"
	if not client.is_file():
		print(f"FAIL: client not found at {client}; build the client first", file=sys.stderr)
		return 2
	if args.only:
		try:
			selection = re.compile(args.only)
		except re.error as error:
			parser.error(f"invalid --only regular expression: {error}")
	else:
		selection = None
	items = staged_demos(build)
	if selection:
		items = [(kind, path) for kind, path in items if selection.search(path.name)]
	if not items:
		print("FAIL: no staged demos matched", file=sys.stderr)
		return 2

	run_name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{os.getpid()}"
	run_dir = build / "demo-check" / run_name
	run_dir.mkdir(parents=True, exist_ok=False)
	results_path = run_dir / "results.json"
	results: list[dict[str, object]] = []
	write_results(results_path, results)
	print(f"checking {len(items)} demo(s), frames={args.frames}; results in {run_dir}", flush=True)
	for index, (kind, demo) in enumerate(items, 1):
		results.append(
			run_demo(
				client,
				run_dir,
				kind,
				demo,
				args.frames,
				args.timeout,
				args.stress_timeout,
				index,
				len(items),
			)
		)
		write_results(results_path, results)

	passed = sum(bool(row["passed"]) for row in results)
	failed = len(results) - passed
	print(f"demo check: {passed}/{len(results)} passed, {failed} failed; {results_path}", flush=True)
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
