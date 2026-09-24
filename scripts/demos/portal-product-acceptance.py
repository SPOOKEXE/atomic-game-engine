#!/usr/bin/env python3
"""Run the supported portal product acceptance rows and record the remaining gaps.

The script deliberately keeps product, protocol, and visual evidence distinct.
The product fixture only exposes 30 and 60 Hz world rates today, while the
seam capture can exercise all four requested presentation rates.  A missing
control remains an ``unsupported`` matrix row and makes ``--mode full`` fail.
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
	unsupported = [entry["name"] for entry in rows if entry["status"] == "unsupported"]
	report = {
		"fixture": "portal-product-acceptance",
		"mode": mode,
		"generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
		"rows": rows,
		"supported_rows_passed": all(entry["status"] != "failed" for entry in rows),
		"unsupported_rows": unsupported,
		"full_acceptance_passed": not unsupported and all(entry["status"] == "passed" for entry in rows),
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


def unsupported_rows(rows: list[dict[str, Any]]) -> None:
	rows.extend(
		[
			row("product-handoff-144fps", "unsupported", reason="PortalWalk exposes only 30 and 60 Hz world rates."),
			row("product-handoff-240fps", "unsupported", reason="PortalWalk exposes only 30 and 60 Hz world rates."),
			row("product-variable-frame-stall", "unsupported", reason="No product fixture controls a deterministic variable frame stall."),
			row("product-visual-impairment-grid", "unsupported", reason="The product fixture has no RTT, jitter, loss, duplicate, or reorder controls."),
			row("product-cpu-gpu-percentiles", "unsupported", reason="Current product sidecars expose counters and snapshots, but no per-frame CPU/GPU percentile series."),
		]
	)


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build", type=Path, default=ROOT / ".cache/build/release-tests")
	parser.add_argument("--out", type=Path)
	parser.add_argument("--mode", choices=("full", "implemented"), default="full")
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
	if args.mode == "full":
		unsupported_rows(rows)
	path = write_report(output, rows, args.mode)
	print(f"portal acceptance report: {path}")
	if args.mode == "full":
		raise SystemExit("full portal acceptance remains incomplete; see unsupported_rows in the report")


if __name__ == "__main__":
	main()
