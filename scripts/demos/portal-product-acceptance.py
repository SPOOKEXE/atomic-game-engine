#!/usr/bin/env python3
"""Run the portal product acceptance matrix and record every row's evidence.

Product, protocol, visual and timing evidence stay distinct rows. Product walks
run at 30 and 60 Hz world rates, at 144 and 240 FPS presentation, under a
variable frame and stall schedule, and across the 24-cell network impairment
grid. Timing rows run the seam scene without image capture, because capture
reads back and writes every frame and would measure disk rather than rendering.
A failing product cell is recorded, not raised, so the report shows every cell;
``full_acceptance_passed`` is true only when every row passed.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
PRODUCT_IMPAIRMENT_PATTERN = re.compile(
	r"portal product impairment rtt_ms=(\d+) jitter_ms=(\d+) loss_percent=(\d+) arrived=(\d+) dropped=(\d+) "
	r"duplicated=(\d+) reordered=(\d+) delayed=(\d+) adoptions=(\d+) awaiting_image_body_frames=(\d+) "
	r"first_contact_frames=(\d+)"
)
FAILURE_PATTERN = re.compile(r"PortalWalk\.cpp:(\d+): failed: (.*?) for:")
IMPAIRMENT_PATTERN = re.compile(
	r"portal process impairment rtt_ms=(\d+) jitter_ms=(\d+) loss_percent=(\d+) "
	r"sample=(\d+) committed_at_tick=(\d+) dropped=(\d+) duplicated=(\d+) reordered=(\d+)"
)
EXPECTED_IMPAIRMENTS = {(rtt, jitter, loss) for rtt in (0, 50, 150, 300) for jitter in (0, 30) for loss in (0, 1, 5)}


def run(command: list[str], output: Path, environment: dict[str, str] | None = None) -> None:
	"""Run one row and retain its complete stdout/stderr in the row directory."""
	output.parent.mkdir(parents=True, exist_ok=True)
	env = None
	if environment:
		env = {**__import__("os").environ, **environment}
	with output.open("w", encoding="utf-8") as log:
		completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, text=True)
	if completed.returncode:
		raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}; see {output}")


def run_status(command: list[str], output: Path, environment: dict[str, str] | None = None) -> int:
	"""Run one row that may legitimately fail and return its exit status."""
	output.parent.mkdir(parents=True, exist_ok=True)
	env = {**__import__("os").environ, **(environment or {})}
	with output.open("w", encoding="utf-8") as log:
		return subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, text=True).returncode


def product_impairment_cells(log: Path) -> list[dict[str, Any]]:
	"""Give each product grid cell the failures reported before its summary line.

	Catch prints a walk's failed assertions as they happen and the walk prints
	its summary after them, so failures belong to the next summary line.
	"""
	cells: list[dict[str, Any]] = []
	failures: dict[str, int] = {}
	for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
		failure = FAILURE_PATTERN.search(line)
		if failure:
			key = f"{failure.group(1)}: {failure.group(2)}"
			failures[key] = failures.get(key, 0) + 1
			continue
		match = PRODUCT_IMPAIRMENT_PATTERN.match(line)
		if not match:
			continue
		values = [int(group) for group in match.groups()]
		names = (
			"rtt_ms",
			"jitter_ms",
			"loss_percent",
			"arrived",
			"dropped",
			"duplicated",
			"reordered",
			"delayed",
			"adoptions",
			"awaiting_image_body_frames",
			"first_contact_frames",
		)
		cells.append({**dict(zip(names, values)), "status": "failed" if failures else "passed", "failures": failures})
		failures = {}
	actual = {(cell["rtt_ms"], cell["jitter_ms"], cell["loss_percent"]) for cell in cells}
	if actual != EXPECTED_IMPAIRMENTS:
		raise RuntimeError(f"product impairment grid differs from requested rows: {sorted(actual)}")
	return sorted(cells, key=lambda cell: (cell["rtt_ms"], cell["jitter_ms"], cell["loss_percent"]))


def percentile(values: list[float], fraction: float) -> float | None:
	"""Nearest rank, matching the frame graph snapshot."""
	if not values:
		return None
	ordered = sorted(values)
	return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]


def timing_summary(csv_path: Path, warmup_frames: int = 60) -> dict[str, Any]:
	"""Achieved rate and p50/p95/p99 from a client --frame-timings series."""
	intervals: list[float] = []
	cpu: list[float] = []
	gpu: list[float] = []
	with csv_path.open(encoding="utf-8") as handle:
		for index, row in enumerate(__import__("csv").DictReader(handle)):
			if index < warmup_frames:
				continue
			if row["interval_ms"]:
				intervals.append(float(row["interval_ms"]))
			cpu.append(float(row["cpu_ms"]))
			if row["gpu_ms"]:
				gpu.append(float(row["gpu_ms"]))
	if not intervals:
		raise RuntimeError(f"{csv_path} has no timed frames after warm-up")

	def stats(values: list[float]) -> dict[str, float | int | None]:
		return {
			"samples": len(values),
			"p50_ms": percentile(values, 0.50),
			"p95_ms": percentile(values, 0.95),
			"p99_ms": percentile(values, 0.99),
			"max_ms": max(values) if values else None,
		}

	return {
		"achieved_fps": 1000.0 * len(intervals) / sum(intervals),
		"interval": stats(intervals),
		"cpu": stats(cpu),
		"gpu": stats(gpu),
	}


def protocol_rows(log: Path) -> list[dict[str, int]]:
	"""Parse the process fixture records and require the declared impairment grid."""
	rows = [
		{
			"rtt_ms": int(match.group(1)),
			"jitter_ms": int(match.group(2)),
			"loss_percent": int(match.group(3)),
			"sample": int(match.group(4)),
			"committed_at_tick": int(match.group(5)),
			"dropped": int(match.group(6)),
			"duplicated": int(match.group(7)),
			"reordered": int(match.group(8)),
		}
		for match in IMPAIRMENT_PATTERN.finditer(log.read_text(encoding="utf-8"))
	]
	actual = {(row["rtt_ms"], row["jitter_ms"], row["loss_percent"]) for row in rows}
	if actual != EXPECTED_IMPAIRMENTS:
		raise RuntimeError(f"protocol grid differs from requested rows: {sorted(actual)}")
	if any(row["sample"] != 0 or row["committed_at_tick"] == 0 for row in rows):
		raise RuntimeError("protocol grid lacks one committed sample per impairment row")
	if not any(row["duplicated"] > 0 for row in rows):
		raise RuntimeError("protocol grid did not observe duplicate injection")
	if not any(row["reordered"] > 0 for row in rows):
		raise RuntimeError("protocol grid did not observe reorder injection")
	return sorted(rows, key=lambda row: (row["rtt_ms"], row["jitter_ms"], row["loss_percent"]))


def row(name: str, status: str, **details: Any) -> dict[str, Any]:
	return {"name": name, "status": status, **details}


def write_report(output: Path, rows: list[dict[str, Any]], mode: str) -> Path:
	failed = [entry["name"] for entry in rows if entry["status"] == "failed"]
	report = {
		"fixture": "portal-product-acceptance",
		"mode": mode,
		"generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
		"rows": rows,
		"failed_rows": failed,
		"full_acceptance_passed": bool(rows) and all(entry["status"] == "passed" for entry in rows),
	}
	path = output / "acceptance-report.json"
	path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
	return path


def capture_visual_rows(build: Path, output: Path, rows: list[dict[str, Any]]) -> None:
	for fps in (30, 60, 144, 240):
		for width, height in ((1920, 1080), (3840, 2160)):
			name = f"seam-matched-{width}x{height}-{fps}fps"
			capture = output / name
			run(
				["scripts/demos/capture-portal-seam.sh", "front"],
				capture / "command.log",
				{
					"PRESET": build.name,
					"OUT": str(capture),
					"WIDTH": str(width),
					"HEIGHT": str(height),
					"FPS": str(fps),
					"SKIP_BUILD": "1",
					"PROFILE_SNAPSHOT": "1",
				},
			)
			report = capture / "portal-seam-report.json"
			if not report.is_file():
				raise RuntimeError(f"{name} did not produce a seam report")
			rows.append(row(name, "passed", fixture="PortalSeam", resolution=f"{width}x{height}", target_fps=fps, report=str(report)))


def run_supported(build: Path, output: Path, rows: list[dict[str, Any]]) -> None:
	test_client = build / "tests/test_client"
	test_script = build / "tests/test_script"
	if not test_client.is_file() or not test_script.is_file():
		raise RuntimeError(f"release test binaries are missing below {build}")

	remote = output / "product-handoff-30-60"
	run(["scripts/demos/profile-portal-remote.sh"], remote / "command.log", {"OUT": str(remote)})
	if not (remote / "remote-profile.json").is_file():
		raise RuntimeError("product handoff profile is absent")
	rows.append(row("product-handoff-30-60", "passed", rates_hz=[30, 60], report=str(remote / "remote-profile.json")))

	for name, filter_text in (
		("product-handoff-matched-30fps", "[portal-product-image-handoff-matched-30fps]"),
		("product-fault-roundtrip", "[portal-product-fault-roundtrip]"),
	):
		log = output / name / "command.log"
		run([str(test_client), filter_text, "--reporter", "compact"], log)
		rows.append(row(name, "passed", log=str(log)))

	protocol_log = output / "protocol-impairment-grid" / "command.log"
	run([str(test_script), "[script][portal-transfer][process]", "--reporter", "compact"], protocol_log)
	parsed = protocol_rows(protocol_log)
	(output / "protocol-impairment-grid" / "rows.json").write_text(json.dumps(parsed, indent=2) + "\n", encoding="utf-8")
	rows.append(row("protocol-impairment-grid", "passed", rows=len(parsed), report=str(protocol_log.with_name("rows.json"))))

	handoff = output / "protocol-handoff-percentiles"
	run(["scripts/demos/profile-portal-handoff.sh"], handoff / "command.log", {"OUT": str(handoff)})
	if not (handoff / "handoff-profile.json").is_file():
		raise RuntimeError("handoff percentile profile is absent")
	rows.append(row("protocol-handoff-percentiles", "passed", repeats=100, report=str(handoff / "handoff-profile.json")))

	capture_visual_rows(build, output, rows)
	product_rows(build, output, rows)
	timing_rows(build, output, rows)


def product_rows(build: Path, output: Path, rows: list[dict[str, Any]]) -> None:
	"""Product walks for the presentation rates, the stall schedule and the impairment grid."""
	test_client = build / "tests/test_client"
	for name, filter_text in (
		("product-handoff-144-240fps", "[portal-product-image-handoff-high-fps]"),
		("product-variable-frame-stall", "[portal-product-frame-stall]"),
	):
		log = output / name / "command.log"
		status = run_status([str(test_client), filter_text, "--reporter", "compact"], log)
		rows.append(row(name, "passed" if status == 0 else "failed", exit_status=status, log=str(log)))

	log = output / "product-impairment-grid" / "command.log"
	status = run_status([str(test_client), "[portal-product-impairment]", "--reporter", "compact"], log)
	cells = product_impairment_cells(log)
	(log.parent / "cells.json").write_text(json.dumps(cells, indent=2) + "\n", encoding="utf-8")
	failed = [f"{cell['rtt_ms']}/{cell['jitter_ms']}/{cell['loss_percent']}" for cell in cells if cell["status"] == "failed"]
	rows.append(
		row(
			"product-impairment-grid",
			"failed" if failed or status != 0 else "passed",
			cells=len(cells),
			failed_cells=failed,
			report=str(log.parent / "cells.json"),
		)
	)


def timing_rows(build: Path, output: Path, rows: list[dict[str, Any]]) -> None:
	"""Uncaptured seam scene timing at every requested rate and resolution."""
	client = build / "client/client"
	scene = build / "assets/examples/scripts/PortalSeam.luau"
	for fps in (30, 60, 144, 240):
		for width, height in ((1920, 1080), (3840, 2160)):
			name = f"seam-timing-{width}x{height}-{fps}fps"
			directory = output / name
			series = directory / "frame-timings.csv"
			run(
				[
					str(client), "--headless", "--uncapped", "--max-fps", str(fps), "--width", str(width),
					"--height", str(height), "--script", str(scene), "--frames", str(fps * 10),
					"--frame-timings", str(series),
				],
				directory / "command.log",
			)
			summary = timing_summary(series)
			(directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
			# The requested rate is a cap; the row records what was achieved beside it.
			rows.append(
				row(
					name,
					"passed" if summary["achieved_fps"] >= 0.95 * fps else "failed",
					target_fps=fps,
					achieved_fps=summary["achieved_fps"],
					resolution=f"{width}x{height}",
					report=str(directory / "summary.json"),
				)
			)


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build", type=Path, default=ROOT / ".cache/build/release-tests")
	parser.add_argument("--out", type=Path)
	parser.add_argument("--mode", choices=("full",), default="full")
	args = parser.parse_args()
	build = args.build.resolve()
	stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
	output = (args.out or build / "portal-product-acceptance" / stamp).resolve()
	if output.exists():
		raise SystemExit(f"refusing to overwrite acceptance artifact directory: {output}")
	output.mkdir(parents=True)
	rows: list[dict[str, Any]] = []
	try:
		run_supported(build, output, rows)
	except RuntimeError as error:
		rows.append(row("runner", "failed", error=str(error)))
		path = write_report(output, rows, args.mode)
		print(f"portal acceptance failed: {path}", file=sys.stderr)
		raise SystemExit(1) from error
	path = write_report(output, rows, args.mode)
	print(f"portal acceptance report: {path}")
	if not json.loads(path.read_text(encoding="utf-8"))["full_acceptance_passed"]:
		raise SystemExit("portal acceptance failed; see failed_rows in the report")


if __name__ == "__main__":
	main()
