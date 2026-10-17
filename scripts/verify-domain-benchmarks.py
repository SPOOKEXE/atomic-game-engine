#!/usr/bin/env python3
"""Checks native CPU domain diagnostics. Timings stay invalid; output stays on stdout."""

from __future__ import annotations

import argparse
import io
import math
import os
import subprocess
import sys
from pathlib import Path

SHAPES = ("empty", "simple", "busy", "complex")
IMAGE_NAMES = {f"b0 image runtime {shape} prepared" for shape in SHAPES}
AUDIO_NAMES = {
	f"b0 audio runtime {shape} prepared {block}"
	for shape in (*SHAPES, "spatial")
	for block in ("first", "next")
}
HEAP_NUMBERS = (
	"process_allocated_bytes",
	"process_allocated_blocks",
	"process_retained_bytes",
	"process_live_bytes",
	"scope_self_peak_bytes",
	"process_live_blocks",
	"process_peak_since_reset_bytes",
	"baseline_live_bytes",
	"process_peak_above_entry_live_bytes",
	"process_header_overhead_model_bytes",
	"block_overhead_bytes",
	"dropped_scopes_delta",
	"foreign_frees_delta",
)


def require(condition: bool, message: str) -> None:
	if not condition:
		raise ValueError(message)


def fields(columns: list[str]) -> dict[str, str]:
	parsed: dict[str, str] = {}
	for column in columns:
		key, separator, value = column.partition("=")
		require(bool(separator) and bool(key) and key not in parsed, f"invalid diagnostic field: {column!r}")
		parsed[key] = value
	return parsed


def number(row: dict[str, str], key: str) -> int:
	require(key in row, f"missing diagnostic counter: {key}")
	return int(row[key])


def equals(row: dict[str, str], expected: dict[str, str]) -> None:
	for key, value in expected.items():
		require(row.get(key) == value, f"{key}: expected {value!r}, got {row.get(key)!r}")


def keyed_rows(output: str, prefix: str, key_columns: int) -> dict[tuple[str, ...], dict[str, str]]:
	rows: dict[tuple[str, ...], dict[str, str]] = {}
	for line in output.splitlines():
		columns = line.split("\t")
		if columns[0] != prefix:
			continue
		require(len(columns) > key_columns, f"incomplete row: {line}")
		key = tuple(columns[1 : 1 + key_columns])
		require(key not in rows, f"duplicate diagnostic row: {key}")
		rows[key] = fields(columns[1 + key_columns :])
	return rows


def check_peak(row: dict[str, str], names: tuple[str, str, str, str, str]) -> None:
	baseline, live, peak, extra, retained = (number(row, name) for name in names)
	require(min(baseline, live, extra) >= 0 and peak >= max(baseline, live), "invalid tracked heap peak")
	require(
		peak - baseline == extra and live - baseline == retained, "inconsistent heap entry/peak/live counters"
	)
	for key in ("dropped_scopes_delta", "foreign_frees_delta"):
		require(number(row, key) == 0, f"nonzero heap diagnostic: {key}")


def check_runtime(output: str, audio: bool) -> None:
	expected_names = AUDIO_NAMES if audio else IMAGE_NAMES
	headers = [line.split("\t")[1:] for line in output.splitlines() if line.startswith("# b0 heap v3\t")]
	require(len(headers) == 1 and headers[0] == ["name", *HEAP_NUMBERS], "missing or changed heap v3 schema")
	basis = [
		fields(line.split("\t")[1:]) for line in output.splitlines() if line.startswith("# b0 heap basis\t")
	]
	require(len(basis) == 1, "expected exactly one runtime heap basis")
	equals(
		basis[0],
		{
			"tracked_cpp_requested_payload": "1",
			"fixtures_prepared": "1",
			"malloc_gpu_excluded": "1",
			"process_peak": "single_call_since_reset",
			"timing_valid": "0",
		},
	)
	rows: dict[str, dict[str, str]] = {}
	for line in output.splitlines():
		columns = line.split("\t")
		if columns[0] != "# b0 heap":
			continue
		require(len(columns) == len(headers[0]) + 1, "heap row does not match v3 schema")
		row = dict(zip(headers[0], columns[1:]))
		name = row.pop("name")
		require(name not in rows, f"duplicate heap row: {name}")
		rows[name] = row
	require(set(rows) == expected_names, f"unexpected heap cohorts: {sorted(rows)}")
	oracles = keyed_rows(output, "# b0 runtime oracle", 1)
	require(set(oracles) == {(name,) for name in expected_names}, "missing or duplicate full runtime oracles")
	for name, row in rows.items():
		for key in HEAP_NUMBERS:
			if key != "process_retained_bytes":
				require(number(row, key) >= 0, f"negative heap counter: {name}: {key}")
		check_peak(
			row,
			(
				"baseline_live_bytes",
				"process_live_bytes",
				"process_peak_since_reset_bytes",
				"process_peak_above_entry_live_bytes",
				"process_retained_bytes",
			),
		)
		require(number(row, "block_overhead_bytes") > 0, "heap hooks did not report tracked block overhead")
		oracle = oracles[(name,)]
		if audio:
			for key in ("process_allocated_bytes", "process_allocated_blocks", "process_retained_bytes"):
				require(
					number(row, key) == 0,
					f"prepared audio block allocated or retained payload: {name}: {key}",
				)
			shape = name.split()[3]
			voices = {"empty": 0, "simple": 1, "busy": 64, "complex": 511, "spatial": 64}[shape]
			begin = 0 if name.endswith("first") else 512
			equals(
				oracle,
				{
					"full_samples": "1",
					"voices": str(voices),
					"frames": "512",
					"spatial": "1" if shape == "spatial" else "0",
					"begin_sample": str(begin),
					"end_sample": str(begin + 512),
				},
			)
			peak = float(oracle.get("peak", "nan"))
			require(
				math.isfinite(peak) and peak >= 0 and oracle.get("clipped") in ("0", "1"),
				"invalid audio peak/clipping report",
			)
			hash_key = "sample_fnv1a64"
		else:
			shape = name.split()[3]
			if shape == "empty":
				equals(oracle, {"refused": "1", "diagnostic": "1", "full_pixels_preserved": "1"})
			else:
				side = {"simple": 1, "busy": 256, "complex": 1024}[shape]
				equals(
					oracle,
					{
						"full_pixels": "1",
						"width": str(side),
						"height": str(side),
						"bytes": str(side * side * 4),
					},
				)
			hash_key = "pixel_fnv1a64"
		require(0 <= number(oracle, hash_key) < 2**64, "invalid complete output hash")


def check_delivery(output: str) -> None:
	rows = keyed_rows(output, "# c06 delivery", 2)
	require(
		set(rows) == {(shape, phase) for shape in SHAPES for phase in ("empty_cache", "warm_cache")},
		"expected eight C06 delivery rows",
	)
	fixtures = keyed_rows(output, "# c06 fixture", 1)
	require(set(fixtures) == {(shape,) for shape in SHAPES}, "expected four C06 authored fixtures")
	basis = [
		fields(line.split("\t")[1:])
		for line in output.splitlines()
		if line.startswith("# c06 delivery basis\t")
	]
	require(len(basis) == 4, "expected four C06 delivery basis rows")
	for row in basis:
		equals(
			row,
			{
				"source": "directory",
				"network_bytes": "not_applicable",
				"filesystem_page_cache": "unspecified",
				"manifest_reads_excluded_from_payload_counters": "1",
				"warm_manifest_read_verified": "1",
				"fresh_client_each_phase": "1",
				"heap_enabled": "1",
				"process_peak": "single_call_since_reset",
			},
		)
	scenario_settings = {
		"empty": (16, 0, 0, 0, 0),
		"simple": (16, 1, 1, 0, 0),
		"busy": (32, 8, 8, 0, 0),
		"complex": (64, 32, 64, 8, 32),
	}
	for shape in SHAPES:
		fixture = fixtures[(shape,)]
		expected_fixture = {"seed": "91", "regions": "2"}
		expected_fixture.update(
			(key, str(value))
			for key, value in zip(
				("side", "villages", "roads", "splines", "overrides"), scenario_settings[shape], strict=True
			)
		)
		equals(fixture, expected_fixture)
		cold, warm = rows[(shape, "empty_cache")], rows[(shape, "warm_cache")]
		for row in (cold, warm):
			equals(
				row,
				{
					"timing_valid": "0",
					"installed": "2",
					"source_failures": "0",
					"verification_failures": "0",
					"refused": "0",
					"pending": "0",
					"full_mesh_object_parity": "1",
				},
			)
			check_peak(
				row,
				(
					"baseline_live_bytes",
					"process_live_bytes",
					"process_peak_since_reset_bytes",
					"process_peak_above_entry_live_bytes",
					"retained_bytes",
				),
			)
			for key in (
				"allocated_bytes",
				"allocated_blocks",
				"live_blocks",
				"header_overhead_model_bytes",
				"cache_hits",
				"cache_misses",
				"bundles",
				"source_bundle_payload_bytes",
				"expanded_bundle_payload_bytes",
			):
				require(number(row, key) >= 0, f"negative C06 counter: {key}")
			require(
				number(row, "block_overhead_bytes") > 0 and 1 <= number(row, "pumps") <= 2000,
				"invalid C06 heap/pump bounds",
			)
			require(
				number(row, "cache_hits") + number(row, "cache_misses") == number(row, "assets_absorbed"),
				"C06 requests and absorbed assets differ",
			)
			for key in ("canonical_asset_hash", "scene_hash"):
				digest = row.get(key, "")
				require(
					len(digest) == 64 and all(char in "0123456789abcdef" for char in digest),
					f"invalid C06 hash: {key}",
				)
				require(row[key] == cold[key], f"C06 cold/warm {key} changed")
		require(
			number(cold, "cache_misses") > 0
			and number(cold, "bundles") > 0
			and number(cold, "source_bundle_payload_bytes") > 0,
			"cold C06 path did not read source bundles",
		)
		require(
			number(cold, "source_bundle_payload_bytes") == number(cold, "expanded_bundle_payload_bytes"),
			"directory bundle payload accounting differs",
		)
		for key in (
			"cache_misses",
			"bundles",
			"source_bundle_payload_bytes",
			"expanded_bundle_payload_bytes",
		):
			require(number(warm, key) == 0, f"warm C06 path touched source bundles: {key}")
		require(
			number(warm, "cache_hits") == number(warm, "assets_absorbed") >= 2,
			"warm C06 path did not hit every request",
		)
		require(
			number(cold, "assets_absorbed") == number(warm, "assets_absorbed"),
			"cold/warm C06 request counts differ",
		)


def emit(output: str | bytes | None) -> None:
	if output:
		text = output.decode("utf-8", errors="replace") if isinstance(output, bytes) else output
		print(text, end="" if text.endswith("\n") else "\n", flush=True)


def main() -> int:
	# grug keep native Windows output compatible with the UTF-8 child capture.
	for stream in (sys.stdout, sys.stderr):
		if isinstance(stream, io.TextIOWrapper):
			stream.reconfigure(encoding="utf-8", errors="replace")
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build-dir", type=Path, default=Path(".cache/build/bench"))
	args = parser.parse_args()
	try:
		build = args.build_dir.resolve()
	except (OSError, ValueError) as failure:
		print(f"invalid build directory {args.build_dir!s}: {failure}", file=sys.stderr)
		return 1
	environment = os.environ.copy()
	environment.update(
		MONO_BENCH_MEMORY="1",
		MONO_PROCEDURAL_DELIVERY_B0="1",
		MONO_PROCEDURAL_DELIVERY_ROOT=str(build / "bench" / "procedural-delivery"),
	)
	if os.name == "nt":
		sdl_runtime = str(build / "mono.vendor" / "sdl")
		environment["PATH"] = os.pathsep.join((sdl_runtime, environment.get("PATH", "")))
	cases = (
		("bench_imagegraph", "engine.imagegraph.bench.evaluate"),
		("bench_audio", "engine.audio.bench.mixing"),
		("bench_unified_tests", "unified.bench.procedural-delivery"),
	)
	print(
		f"# native domain diagnostics\tplatform={sys.platform}\ttiming_valid=0\tbuild_dir={build}",
		flush=True,
	)
	timed_out = False
	failed = False
	for binary, suite in cases:
		executable = build / "bench" / (binary + (".exe" if os.name == "nt" else ""))
		print(f"# native domain suite\t{suite}\ttimeout_seconds=180", flush=True)
		try:
			require(executable.is_file(), f"missing native benchmark executable: {executable}")
			command = [str(executable), "--suite", suite, "--samples", "1", "--warmup-samples", "0"]
			result = subprocess.run(
				command,
				cwd=Path(__file__).resolve().parents[1],
				env=environment,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				encoding="utf-8",
				errors="replace",
				timeout=180,
				check=False,
			)
			emit(result.stdout)
			require(
				result.returncode == 0,
				f"native benchmark failed: {suite}: child_returncode={result.returncode}",
			)
			if binary == "bench_unified_tests":
				check_delivery(result.stdout)
			else:
				check_runtime(result.stdout, audio=binary == "bench_audio")
			print(f"# native domain verified\t{suite}\ttiming_valid=0", flush=True)
		except subprocess.TimeoutExpired as failure:
			emit(failure.output)
			print(
				f"native domain suite timed out after {failure.timeout}s: {suite}: {failure.cmd}",
				file=sys.stderr,
			)
			timed_out = True
		except (OSError, ValueError) as failure:
			print(f"native domain suite failed: {suite}: {failure}", file=sys.stderr)
			failed = True
	return 124 if timed_out else 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
