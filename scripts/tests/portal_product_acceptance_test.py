#!/usr/bin/env python3
"""Focused checks for portal-product-acceptance.py's matrix accounting."""

from __future__ import annotations

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import json
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = spec_from_file_location("portal_product_acceptance", ROOT / "scripts/demos/portal-product-acceptance.py")
if SPEC is None or SPEC.loader is None:
	raise RuntimeError("could not load scripts/demos/portal-product-acceptance.py")
MODULE = module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class PortalProductAcceptanceTest(unittest.TestCase):
	def test_protocol_grid_requires_all_requested_impairments_and_fault_evidence(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			log = Path(temporary) / "protocol.log"
			lines = []
			for rtt, jitter, loss in sorted(MODULE.EXPECTED_IMPAIRMENTS):
				lines.append(
					"portal process impairment "
					f"rtt_ms={rtt} jitter_ms={jitter} loss_percent={loss} sample=0 committed_at_tick=10 "
					f"dropped={loss} duplicated=1 reordered={1 if jitter else 0}"
				)
			log.write_text("\n".join(lines) + "\n", encoding="utf-8")
			rows = MODULE.protocol_rows(log)
			self.assertEqual(len(rows), 24)
			self.assertEqual(rows[0]["rtt_ms"], 0)

	def test_protocol_grid_rejects_a_missing_row(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			log = Path(temporary) / "protocol.log"
			log.write_text(
				"portal process impairment rtt_ms=0 jitter_ms=0 loss_percent=0 sample=0 committed_at_tick=1 dropped=0 duplicated=1 reordered=1\n",
				encoding="utf-8",
			)
			with self.assertRaises(RuntimeError):
				MODULE.protocol_rows(log)

	def test_product_grid_assigns_failures_to_the_following_cell(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			log = Path(temporary) / "product.log"
			lines = []
			for rtt, jitter, loss in sorted(MODULE.EXPECTED_IMPAIRMENTS):
				if (rtt, jitter, loss) == (150, 0, 0):
					lines.append("/x/PortalWalk.cpp:1117: failed: sample.value(\"eye_image\", false) for: false")
					lines.append("/x/PortalWalk.cpp:1117: failed: sample.value(\"eye_image\", false) for: false")
				lines.append(
					"portal product impairment "
					f"rtt_ms={rtt} jitter_ms={jitter} loss_percent={loss} arrived=10 dropped={loss} "
					f"duplicated=1 reordered=1 delayed={rtt} adoptions=2"
				)
			log.write_text("\n".join(lines) + "\n", encoding="utf-8")
			cells = MODULE.product_impairment_cells(log)
			failed = [cell for cell in cells if cell["status"] == "failed"]
			self.assertEqual(len(cells), 24)
			self.assertEqual([(cell["rtt_ms"], cell["jitter_ms"], cell["loss_percent"]) for cell in failed], [(150, 0, 0)])
			self.assertEqual(failed[0]["failures"], {"1117: sample.value(\"eye_image\", false)": 2})

	def test_timing_summary_skips_warmup_and_reports_the_achieved_rate(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			series = Path(temporary) / "frame-timings.csv"
			rows = ["frame,interval_ms,cpu_ms,gpu_ms", "0,,30,"]
			rows += [f"{index},4,0.5,{'0.3' if index % 4 == 0 else ''}" for index in range(1, 201)]
			series.write_text("\n".join(rows) + "\n", encoding="utf-8")
			summary = MODULE.timing_summary(series, warmup_frames=10)
			self.assertAlmostEqual(summary["achieved_fps"], 250.0)
			self.assertEqual(summary["interval"]["p99_ms"], 4.0)
			self.assertEqual(summary["gpu"]["samples"], 48)

	def test_a_failed_row_blocks_full_acceptance(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			rows = [MODULE.row("passing", "passed"), MODULE.row("product-impairment-grid", "failed")]
			report = json.loads(MODULE.write_report(Path(temporary), rows, "full").read_text(encoding="utf-8"))
			self.assertFalse(report["full_acceptance_passed"])
			self.assertEqual(report["failed_rows"], ["product-impairment-grid"])


if __name__ == "__main__":
	unittest.main()
