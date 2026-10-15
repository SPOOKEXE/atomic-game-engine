#!/usr/bin/env python3
"""Reports retained build history, or profiles one command's build activity.

Ninja retains one entry per output across invocations. Its log records elapsed
time for each edge, not the dependency critical path. Edges overlap, so grouped
totals are compiler work and can exceed the retained-log span. The report keeps
that distinction visible rather than presenting a sum of parallel work as
elapsed build time.

`CCACHE_STATSLOG` has one comment-delimited block per compiler invocation. It
records the cache result for the translation unit named by that comment. CMake
unity builds cache the generated unity source as one unit; the report expands
that source to its authored .cpp includes for attribution only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

UNITY_INCLUDE = re.compile(r'^\s*#include\s+"([^"]+\.cpp)"\s*$')


@dataclass(slots=True)
class BuildEdge:
	"""One completed Ninja edge, with timestamps retained in Ninja's log."""

	start_milliseconds: int
	end_milliseconds: int
	output: str
	command_hash: str
	outputs: tuple[str, ...]

	@property
	def elapsed_milliseconds(self) -> int:
		return self.end_milliseconds - self.start_milliseconds

	@property
	def output_count(self) -> int:
		return len(self.outputs)


@dataclass(slots=True)
class CacheOutcome:
	"""Cumulative cache outcomes for one ccache translation unit."""

	translation_unit: str
	hits: int = 0
	misses: int = 0
	uncacheable: int = 0
	reasons: dict[str, int] = field(default_factory=dict)

	@property
	def calls(self) -> int:
		return self.hits + self.misses + self.uncacheable

	@property
	def cacheable_calls(self) -> int:
		return self.hits + self.misses

	@property
	def hit_rate(self) -> float:
		return 100.0 * self.hits / self.cacheable_calls if self.cacheable_calls else 0.0

	@property
	def miss_rate(self) -> float:
		return 100.0 * self.misses / self.cacheable_calls if self.cacheable_calls else 0.0


def parse_ninja_log(lines: Iterable[str]) -> list[BuildEdge]:
	"""Coalesces Ninja v5 rows from the selected log interval."""

	commands: dict[tuple[int, int, str], list[str]] = {}
	for line in lines:
		if not line or line.startswith("#"):
			continue
		fields = line.split("\t")
		if len(fields) < 5:
			continue
		try:
			start = int(fields[0])
			end = int(fields[1])
		except ValueError:
			continue
		if end < start:
			continue
		commands.setdefault((start, end, fields[4]), []).append(fields[3])

	edges: list[BuildEdge] = []
	for (start, end, command_hash), outputs in commands.items():
		all_outputs = tuple(sorted(set(outputs)))
		edges.append(
			BuildEdge(
				start_milliseconds=start,
				end_milliseconds=end,
				output=min(all_outputs, key=lambda output: (len(output), output)),
				command_hash=command_hash,
				outputs=all_outputs,
			)
		)
	return edges


def read_ninja_log(path: Path) -> list[BuildEdge]:
	"""Reads retained Ninja v5 rows."""

	return parse_ninja_log(path.read_text(encoding="utf-8", errors="replace").splitlines())


def new_ninja_rows(before: bytes | None, after: bytes | None) -> list[str]:
	"""Finds new rows even when Ninja compacts its retained output history."""

	if after is None:
		return []
	if before is None:
		delta = after
	elif after.startswith(before):
		delta = after[len(before):]
	else:
		# A rewritten log can retain older rows for an output with newly stamped mtimes.
		# Only its last row represents the latest completed edge for that output.
		latest_rows: dict[bytes, bytes] = {}
		for row in after.splitlines():
			fields = row.split(b"\t")
			if len(fields) == 5:
				latest_rows[fields[3]] = row
		# Keep the raw mtime column. Equal durations and hashes can occur in later builds.
		previous = Counter(before.splitlines())
		new_rows = []
		for row in latest_rows.values():
			if previous[row]:
				previous[row] -= 1
			else:
				new_rows.append(row.decode("utf-8", errors="replace"))
		return new_rows
	return delta.decode("utf-8", errors="replace").splitlines()


def build_span(edges: Iterable[BuildEdge]) -> tuple[int, int] | None:
	"""Returns the interval covered by a set of retained Ninja log entries."""

	edges = list(edges)
	if not edges:
		return None
	return min(edge.start_milliseconds for edge in edges), max(edge.end_milliseconds for edge in edges)


def group_for(path: str) -> str:
	"""Maps an output path to a stable module or vendor group."""

	parts = PurePosixPath(path.replace("\\", "/")).parts
	for index, part in enumerate(parts):
		if part == "mono.vendor" and index + 1 < len(parts):
			return f"mono.vendor/{parts[index + 1]}"
		if part == "mono.engine" and index + 1 < len(parts):
			return f"mono.engine/{parts[index + 1]}"
		if part.startswith("mono."):
			return part
	return "other"


def source_for_output(build_directory: Path, output: str) -> Path | None:
	"""Finds the generated source corresponding to a compiler object output."""

	candidate = Path(output)
	if not candidate.is_absolute():
		candidate = build_directory / candidate
	if candidate.suffix in {".o", ".obj"}:
		candidate = candidate.with_suffix("")
	return candidate if candidate.is_file() else None


def unity_sources(source: Path | None) -> list[str]:
	"""Expands a CMake unity source to authored C++ sources when possible."""

	if source is None:
		return []
	try:
		text = source.read_text(encoding="utf-8", errors="replace")
	except OSError:
		return []
	if not text.startswith("/* generated by CMake */"):
		return []
	return [match.group(1) for line in text.splitlines() if (match := UNITY_INCLUDE.match(line))]


def display_edge(build_directory: Path, edge: BuildEdge) -> str:
	"""Describes an edge and expands unity inputs without changing cache granularity."""

	sources = unity_sources(source_for_output(build_directory, edge.output))
	if not sources:
		label = edge.output
	else:
		names = ", ".join(Path(source).name for source in sources)
		label = f"{edge.output} [unity unit: {names}]"
	if edge.output_count > 1:
		return f"{label} [{edge.output_count} outputs]"
	return label


CCACHE_HIT_EVENTS = {"direct_cache_hit", "preprocessed_cache_hit"}
CCACHE_AUXILIARY_EVENTS = CCACHE_HIT_EVENTS | {
	"cache_miss",
	"local_storage_hit",
	"local_storage_miss",
	"local_storage_read_hit",
	"local_storage_read_miss",
	"local_storage_write",
	"remote_storage_hit",
	"remote_storage_miss",
	"remote_storage_read_hit",
	"remote_storage_read_miss",
	"remote_storage_write",
}


def parse_ccache_stats(lines: Iterable[str]) -> dict[str, CacheOutcome]:
	"""Accumulates terminal ccache hits and misses from comment-delimited blocks."""

	results: dict[str, CacheOutcome] = {}
	current: str | None = None
	events: set[str] = set()

	def finish() -> None:
		if current is None:
			return
		outcome = results.setdefault(current, CacheOutcome(current))
		if CCACHE_HIT_EVENTS & events:
			outcome.hits += 1
		elif "cache_miss" in events:
			outcome.misses += 1
		else:
			outcome.uncacheable += 1
			reasons = sorted(events - CCACHE_AUXILIARY_EVENTS)
			for reason in reasons or ["unknown_outcome"]:
				outcome.reasons[reason] = outcome.reasons.get(reason, 0) + 1

	for raw in lines:
		line = raw.strip()
		if line.startswith("#"):
			finish()
			current = line[1:].strip() or None
			events = set()
		elif current is not None and line:
			events.add(line)
	finish()
	return results


def read_ccache_stats(path: Path) -> dict[str, CacheOutcome]:
	"""Reads cumulative ccache outcomes."""

	return parse_ccache_stats(path.read_text(encoding="utf-8", errors="replace").splitlines())


def source_for_cache_unit(build_directory: Path, translation_unit: str) -> Path | None:
	"""Resolves ccache's source label in the build tree when it is relative."""

	path = Path(translation_unit)
	if not path.is_absolute():
		path = build_directory / path
	return path if path.is_file() else None


def display_cache_unit(build_directory: Path, outcome: CacheOutcome) -> str:
	"""Displays one cache unit and its authored sources without dividing the event."""

	sources = unity_sources(source_for_cache_unit(build_directory, outcome.translation_unit))
	if not sources:
		return outcome.translation_unit
	names = ", ".join(Path(source).name for source in sources)
	return f"{outcome.translation_unit} [unity unit: {names}]"


def print_edges(build_directory: Path, edges: list[BuildEdge], top: int, current_run: bool = False) -> None:
	"""Prints edge duration, grouped-work and final-tail views."""

	span = build_span(edges)
	if span is None:
		print("ninja: no completed edges" + (" in this run" if current_run else ""))
		return
	start, end = span
	scope = "this-run" if current_run else "retained-log"
	print(f"ninja {scope} span: {(end - start) / 1000.0:.3f} s across {len(edges)} completed edges")
	if not current_run:
		print("Ninja retains output history across invocations. This is a single-build span only in a fresh tree, such as CI.")
	print("edge durations are not a dependency critical path; parallel work overlaps.")
	print()
	print(f"top {top} {scope} edges by elapsed time")
	for edge in sorted(edges, key=lambda value: value.elapsed_milliseconds, reverse=True)[:top]:
		print(f"  {edge.elapsed_milliseconds / 1000.0:8.3f} s  {display_edge(build_directory, edge)}")

	work: dict[str, int] = {}
	for edge in edges:
		group = group_for(edge.output)
		work[group] = work.get(group, 0) + edge.elapsed_milliseconds
	print()
	print(f"top {top} groups by {scope} cumulative edge work")
	for group, milliseconds in sorted(work.items(), key=lambda item: item[1], reverse=True)[:top]:
		print(f"  {milliseconds / 1000.0:8.3f} s  {group}")

	tail_start = start + (end - start) * 9 // 10
	tail = [edge for edge in edges if edge.end_milliseconds >= tail_start]
	print()
	print(f"top {top} edges ending in the final 10% of the {scope} span")
	for edge in sorted(tail, key=lambda value: value.elapsed_milliseconds, reverse=True)[:top]:
		print(f"  {edge.elapsed_milliseconds / 1000.0:8.3f} s  {display_edge(build_directory, edge)}")


def print_cache(
	build_directory: Path,
	stats_log: Path,
	top: int,
	outcomes: dict[str, CacheOutcome] | None = None,
	current_run: bool = False,
) -> None:
	"""Prints ccache results without treating a cache hit as source-level work."""

	if outcomes is None and not stats_log.is_file():
		print()
		print(f"ccache: no stats log at {stats_log}")
		return
	if outcomes is None:
		outcomes = read_ccache_stats(stats_log)
	measured = [outcome for outcome in outcomes.values() if outcome.calls]
	if not measured:
		print()
		message = (
			"ccache: no recorded calls in this run"
			if current_run
			else "ccache: stats log contains no terminal hit or miss outcomes"
		)
		print(message)
		return
	total_hits = sum(outcome.hits for outcome in measured)
	total_misses = sum(outcome.misses for outcome in measured)
	total_uncacheable = sum(outcome.uncacheable for outcome in measured)
	total_cacheable = total_hits + total_misses
	total_calls = total_cacheable + total_uncacheable
	print()
	hit_rate = 100.0 * total_hits / total_cacheable if total_cacheable else 0.0
	print(
		f"ccache {'this run' if current_run else 'cumulative'}: {total_hits} hits, "
		f"{total_misses} misses, {total_uncacheable} uncacheable "
		f"across {total_calls} compiler calls, {hit_rate:.1f}% cacheable hit rate")
	print("cache granularity is the translation unit. Unity source lists are attribution only.")
	print("translation units that always miss (at least 2 calls)")
	always_misses = [
		outcome for outcome in measured if outcome.calls >= 2 and outcome.misses == outcome.calls
	]
	if not always_misses:
		print("  none")
	for outcome in sorted(
		always_misses,
		key=lambda value: (value.miss_rate, value.misses),
		reverse=True,
	)[:top]:
		print(
			f"  {outcome.hits:5d} hits  {outcome.misses:5d} misses  {outcome.uncacheable:5d} uncacheable  "
			f"{outcome.miss_rate:6.1f}% miss  "
			f"{display_cache_unit(build_directory, outcome)}"
		)
	print("translation units repeatedly uncacheable (at least 2 uncacheable calls)")
	repeated_uncacheable = [outcome for outcome in measured if outcome.uncacheable >= 2]
	if not repeated_uncacheable:
		print("  none")
	for outcome in sorted(
		repeated_uncacheable,
		key=lambda value: (value.uncacheable, value.calls),
		reverse=True,
	)[:top]:
		reason_labels = ", ".join(
			f"{reason} ({count})" for reason, count in sorted(outcome.reasons.items(), key=lambda item: (-item[1], item[0]))
		)
		print(
			f"  {outcome.uncacheable:5d} uncacheable  {reason_labels}  "
			f"{display_cache_unit(build_directory, outcome)}"
		)
	reasons: dict[str, int] = {}
	for outcome in measured:
		for reason, count in outcome.reasons.items():
			reasons[reason] = reasons.get(reason, 0) + count
	print(f"top {top} uncacheable terminal reasons")
	if not reasons:
		print("  none")
	for reason, count in sorted(reasons.items(), key=lambda item: (-item[1], item[0]))[:top]:
		print(f"  {count:5d}  {reason}")
	print(f"top {top} translation units by {'this-run' if current_run else 'cumulative'} misses")
	misses = [outcome for outcome in measured if outcome.misses > 0]
	if not misses:
		print("  none")
	for outcome in sorted(misses, key=lambda value: (value.misses, value.calls), reverse=True)[:top]:
		print(
			f"  {outcome.hits:5d} hits  {outcome.misses:5d} misses  {outcome.uncacheable:5d} uncacheable  "
			f"{outcome.hit_rate:6.1f}%  "
			f"{display_cache_unit(build_directory, outcome)}"
		)


def read_log_bytes(path: Path) -> bytes | None:
	try:
		return path.read_bytes()
	except FileNotFoundError:
		return None


def profile_run(build_directory: Path, stats_log: Path, top: int, command: list[str]) -> int:
	"""Runs one command and reports only its newly recorded build activity."""

	ninja_log = build_directory / ".ninja_log"
	previous_ninja = read_log_bytes(ninja_log)
	previous_stats = read_log_bytes(stats_log)
	start = time.monotonic()
	try:
		status = subprocess.run(command, check=False).returncode
	except OSError as error:
		print(f"build-profile: could not run {command[0]}: {error}", file=sys.stderr)
		status = 127
	wall_seconds = time.monotonic() - start
	print(f"build wall time: {wall_seconds:.3f} s")
	try:
		current_ninja = read_log_bytes(ninja_log)
		current_stats = read_log_bytes(stats_log)
		edges = parse_ninja_log(new_ninja_rows(previous_ninja, current_ninja))
		print_edges(build_directory, edges, top, current_run=True)
		if current_stats is None:
			print(f"\nccache: no stats log at {stats_log}")
		elif previous_stats is not None and not current_stats.startswith(previous_stats):
			print("\nccache: stats log was replaced; this-run outcomes cannot be determined")
		else:
			delta = current_stats[len(previous_stats):] if previous_stats is not None else current_stats
			outcomes = parse_ccache_stats(delta.decode("utf-8", errors="replace").splitlines())
			print_cache(build_directory, stats_log, top, outcomes, current_run=True)
	except (OSError, ValueError) as error:
		print(f"build-profile: report failed: {error}", file=sys.stderr)
		return status if status else 1
	return status


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--build-dir", type=Path, required=True, help="CMake/Ninja build directory")
	parser.add_argument("--stats-log", type=Path, help="CCACHE_STATSLOG path, defaulting inside build-dir")
	parser.add_argument("--top", type=int, default=5, help="rows to print in each ranked section")
	parser.add_argument("--run", nargs=argparse.REMAINDER, help="command and arguments to profile")
	args = parser.parse_args()
	if args.top <= 0:
		parser.error("--top must be positive")
	if args.run is not None and not args.run:
		parser.error("--run requires a command")

	build_directory = args.build_dir
	ninja_log = build_directory / ".ninja_log"
	stats_log = args.stats_log or build_directory / "ccache-stats.log"
	if args.run is not None:
		return profile_run(build_directory, stats_log, args.top, args.run)
	if not ninja_log.is_file():
		print(f"ninja: no log at {ninja_log}")
	else:
		print_edges(build_directory, read_ninja_log(ninja_log), args.top)
	print_cache(build_directory, stats_log, args.top)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
