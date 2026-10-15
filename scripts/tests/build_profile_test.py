#!/usr/bin/env python3
"""Focused checks for the standalone Ninja and ccache build profiler."""

from __future__ import annotations

import contextlib
import subprocess
import sys
import tempfile
import unittest
from importlib.util import module_from_spec, spec_from_file_location
from io import StringIO
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = spec_from_file_location("build_profile", ROOT / "scripts" / "build-profile.py")
if SPEC is None or SPEC.loader is None:
	raise RuntimeError("could not load scripts/build-profile.py")
MODULE = module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class BuildProfileTest(unittest.TestCase):
	def test_run_reports_only_new_edges_and_cache_calls(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			ninja = build / ".ninja_log"
			stats = build / "ccache-stats.log"
			ninja.write_text("# ninja log v5\n1\t11\t100\told.o\thash\n", encoding="utf-8")
			stats.write_text("# old.cpp\ncache_miss\n", encoding="utf-8")

			def build_command(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
				self.assertEqual((command, check), (["cmake", "--build", "tree"], False))
				with ninja.open("a", encoding="utf-8") as log:
					log.write("12\t42\t200\tnew.o\thash\n")
				with stats.open("a", encoding="utf-8") as log:
					log.write("# new.cpp\ndirect_cache_hit\n")
				return subprocess.CompletedProcess(command, 0)

			output = StringIO()
			argv = ["build-profile.py", "--build-dir", str(build), "--run", "cmake", "--build", "tree"]
			with (
				patch.object(sys, "argv", argv),
				patch.object(MODULE.subprocess, "run", side_effect=build_command),
				contextlib.redirect_stdout(output),
			):
				self.assertEqual(MODULE.main(), 0)
			text = output.getvalue()
			self.assertIn("build wall time:", text)
			self.assertIn("this-run span: 0.030 s across 1 completed edges", text)
			self.assertIn("new.o", text)
			self.assertNotIn("old.o", text)
			self.assertIn("ccache this run: 1 hits, 0 misses", text)
			self.assertNotIn("old.cpp", text)

	def test_null_build_has_no_new_activity(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			(build / ".ninja_log").write_text("# ninja log v5\n1\t11\t100\told.o\thash\n", encoding="utf-8")
			(build / "ccache-stats.log").write_text("# old.cpp\ncache_miss\n", encoding="utf-8")
			output = StringIO()
			with (
				patch.object(MODULE.subprocess, "run", return_value=subprocess.CompletedProcess(["true"], 0)),
				contextlib.redirect_stdout(output),
			):
				self.assertEqual(MODULE.profile_run(build, build / "ccache-stats.log", 5, ["true"]), 0)
			self.assertIn("ninja: no completed edges in this run", output.getvalue())
			self.assertIn("ccache: no recorded calls in this run", output.getvalue())

	def test_compacted_ninja_log_uses_raw_mtime_to_find_rebuilt_edge(self) -> None:
		before = b"# ninja log v5\n1\t11\t100\trebuilt.o\thash\n2\t12\t101\tkept.o\tother\n"
		after = b"# ninja log v5\n2\t12\t101\tkept.o\tother\n1\t11\t200\trebuilt.o\thash\n"
		edges = MODULE.parse_ninja_log(MODULE.new_ninja_rows(before, after))
		self.assertEqual(len(edges), 1)
		self.assertEqual(edges[0].output, "rebuilt.o")

	def test_rewritten_ninja_log_uses_only_latest_row_per_output(self) -> None:
		before = (
			b"# ninja log v5\n"
			b"72\t1450\t100\tbuild.ninja\thash\n"
			b"52\t1356\t100\tbuild.ninja\thash\n"
		)
		after = (
			b"# ninja log v5\n"
			b"52\t1356\t200\tbuild.ninja\thash\n"
			b"52\t1274\t200\tbuild.ninja\thash\n"
		)
		expected = ["52\t1274\t200\tbuild.ninja\thash"]
		self.assertEqual(MODULE.new_ninja_rows(before, after), expected)
		edges = MODULE.parse_ninja_log(MODULE.new_ninja_rows(before, after))
		self.assertEqual([edge.elapsed_milliseconds for edge in edges], [1222])

		unchanged = b"9\t19\t300\tkept.o\tother\n"
		self.assertEqual(MODULE.new_ninja_rows(before + unchanged, after + unchanged), expected)

	def test_failed_command_reports_partial_logs_and_keeps_status(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			def failed_command(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
				(build / ".ninja_log").write_text("# ninja log v5\n1\t11\t100\tpartial.o\thash\n", encoding="utf-8")
				(build / "ccache-stats.log").write_text("# partial.cpp\ncache_miss\n", encoding="utf-8")
				return subprocess.CompletedProcess(command, 42)

			output = StringIO()
			with (
				patch.object(MODULE.subprocess, "run", side_effect=failed_command),
				contextlib.redirect_stdout(output),
			):
				self.assertEqual(MODULE.profile_run(build, build / "ccache-stats.log", 5, ["false"]), 42)
			self.assertIn("partial.o", output.getvalue())
			self.assertIn("ccache this run: 0 hits, 1 misses", output.getvalue())

			with (
				patch.object(MODULE.subprocess, "run", return_value=subprocess.CompletedProcess(["false"], 42)),
				patch.object(MODULE, "print_edges", side_effect=OSError("report unavailable")),
				contextlib.redirect_stderr(StringIO()),
			):
				self.assertEqual(MODULE.profile_run(build, build / "ccache-stats.log", 5, ["false"]), 42)

	def test_replaced_ccache_log_is_not_counted_as_current_activity(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			stats = build / "ccache-stats.log"
			stats.write_text("# previous.cpp\ncache_miss\n", encoding="utf-8")
			def replace_log(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
				stats.write_text("# unknown.cpp\ndirect_cache_hit\n", encoding="utf-8")
				return subprocess.CompletedProcess(command, 0)

			output = StringIO()
			with patch.object(MODULE.subprocess, "run", side_effect=replace_log), contextlib.redirect_stdout(output):
				self.assertEqual(MODULE.profile_run(build, stats, 5, ["true"]), 0)
			self.assertIn("stats log was replaced", output.getvalue())
			self.assertNotIn("unknown.cpp", output.getvalue())

	def test_standalone_mode_keeps_historical_report(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			(build / ".ninja_log").write_text("# ninja log v5\n1\t11\t100\told.o\thash\n", encoding="utf-8")
			(build / "ccache-stats.log").write_text("# old.cpp\ncache_miss\n", encoding="utf-8")
			output = StringIO()
			with (
				patch.object(sys, "argv", ["build-profile.py", "--build-dir", str(build)]),
				contextlib.redirect_stdout(output),
			):
				self.assertEqual(MODULE.main(), 0)
			self.assertIn("ninja retained-log span", output.getvalue())
			self.assertIn("ccache cumulative: 0 hits, 1 misses", output.getvalue())

	def test_ninja_profile_expands_unity_sources_and_groups_work(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			unity = build / "mono.client/CMakeFiles/client_lib.dir/Unity/unity_0_cxx.cxx"
			unity.parent.mkdir(parents=True)
			unity.write_text(
				"/* generated by CMake */\n#include \"/repo/mono.client/src/Client.cpp\"\n"
				"#include \"/repo/mono.client/src/Scene.cpp\"\n",
				encoding="utf-8",
			)
			(build / ".ninja_log").write_text(
				"# ninja log v5\n"
				"10\t80\t0\tmono.client/CMakeFiles/client_lib.dir/Unity/unity_0_cxx.cxx.o\thash\n"
				"20\t45\t0\tmono.vendor/sqlitecpp/sqlite3/CMakeFiles/sqlite3.dir/sqlite3.c.o\tsqlite-hash\n"
				+ "".join(
					f"100\t150\t0\tmono.tools/generated/output-{index}.txt\tmulti-output-hash\n"
					for index in range(268)
				),
				encoding="utf-8",
			)

			edges = MODULE.read_ninja_log(build / ".ninja_log")
			self.assertEqual(len(edges), 3)
			multi_output = next(edge for edge in edges if edge.command_hash == "multi-output-hash")
			self.assertEqual(multi_output.output_count, 268)
			self.assertEqual(sum(edge.elapsed_milliseconds for edge in edges), 145)

			output = StringIO()
			with contextlib.redirect_stdout(output):
				MODULE.print_edges(build, edges, 3)
			text = output.getvalue()
			self.assertIn("0.140 s", text)
			self.assertIn("retained-log span", text)
			self.assertIn("retains output history across invocations", text)
			self.assertIn("unity unit: Client.cpp, Scene.cpp", text)
			self.assertIn("[268 outputs]", text)
			self.assertIn("mono.client", text)
			self.assertIn("mono.vendor/sqlitecpp", text)
			self.assertIn("not a dependency critical path", text)

	def test_ccache_stats_accumulate_terminal_outcomes_per_unity_unit(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			build = Path(temporary)
			unity = build / "mono.engine/core/CMakeFiles/engine_core.dir/Unity/unity_0_cxx.cxx"
			unity.parent.mkdir(parents=True)
			unity.write_text(
				"/* generated by CMake */\n#include \"/repo/mono.engine/core/src/Log.cpp\"\n",
				encoding="utf-8",
			)
			stats = build / "ccache-stats.log"
			stats.write_text(
				"# mono.engine/core/CMakeFiles/engine_core.dir/Unity/unity_0_cxx.cxx\n"
				"direct_cache_hit\nlocal_storage_hit\n"
				"# mono.engine/core/CMakeFiles/engine_core.dir/Unity/unity_0_cxx.cxx\n"
				"cache_miss\nlocal_storage_miss\n"
				"# mono.vendor/miniz/miniz.c\npreprocessed_cache_hit\n"
				"# mono.vendor/unlucky/unlucky.c\ncache_miss\n"
				"# mono.vendor/unlucky/unlucky.c\ncache_miss\n"
				"# mono.vendor/doomed/doomed.c\ncache_miss\n"
				"# mono.vendor/doomed/doomed.c\ncache_miss\n"
				"# mono.vendor/doomed/doomed.c\ncache_miss\n"
				"# mono.tools/uncacheable/flags.cpp\nunsupported_compiler_option\n"
				"# mono.tools/uncacheable/flags.cpp\nunsupported_compiler_option\n"
				"# mono.tools/uncacheable/broken.cpp\ncompiler_error\n",
				encoding="utf-8",
			)

			outcomes = MODULE.read_ccache_stats(stats)
			unity_outcome = outcomes["mono.engine/core/CMakeFiles/engine_core.dir/Unity/unity_0_cxx.cxx"]
			self.assertEqual((unity_outcome.hits, unity_outcome.misses), (1, 1))
			self.assertEqual(outcomes["mono.vendor/miniz/miniz.c"].hits, 1)
			self.assertEqual(outcomes["mono.vendor/unlucky/unlucky.c"].misses, 2)
			self.assertEqual(outcomes["mono.vendor/doomed/doomed.c"].misses, 3)
			flags = outcomes["mono.tools/uncacheable/flags.cpp"]
			self.assertEqual((flags.hits, flags.misses, flags.uncacheable), (0, 0, 2))
			self.assertEqual(flags.reasons, {"unsupported_compiler_option": 2})
			self.assertEqual(outcomes["mono.tools/uncacheable/broken.cpp"].reasons, {"compiler_error": 1})

			output = StringIO()
			with contextlib.redirect_stdout(output):
				MODULE.print_cache(build, stats, 3)
			text = output.getvalue()
			self.assertIn("2 hits, 6 misses, 3 uncacheable across 11 compiler calls, 25.0%", text)
			self.assertIn("unity unit: Log.cpp", text)
			self.assertIn("attribution only", text)
			self.assertIn("translation units that always miss (at least 2 calls)", text)
			self.assertIn("mono.vendor/unlucky/unlucky.c", text)
			self.assertIn("mono.vendor/doomed/doomed.c", text)
			self.assertLess(text.index("mono.vendor/doomed/doomed.c"), text.index("mono.vendor/unlucky/unlucky.c"))
			self.assertIn("translation units repeatedly uncacheable", text)
			self.assertIn("mono.tools/uncacheable/flags.cpp", text)
			self.assertIn("unsupported_compiler_option (2)", text)
			self.assertIn("compiler_error", text)

			hits_only = build / "hits-only.log"
			hits_only.write_text(
				"# mono.vendor/clean/clean.cpp\n"
				"direct_cache_hit\n",
				encoding="utf-8",
			)
			hits_output = StringIO()
			with contextlib.redirect_stdout(hits_output):
				MODULE.print_cache(build, hits_only, 3)
			self.assertIn("top 3 translation units by cumulative misses\n  none", hits_output.getvalue())


if __name__ == "__main__":
	unittest.main()
