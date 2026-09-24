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

	def test_full_report_names_unsupported_rows_and_cannot_pass(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			output = Path(temporary)
			rows = [MODULE.row("supported", "passed")]
			MODULE.unsupported_rows(rows)
			report_path = MODULE.write_report(output, rows, "full")
			report = json.loads(report_path.read_text(encoding="utf-8"))
			self.assertFalse(report["full_acceptance_passed"])
			self.assertIn("product-variable-frame-stall", report["unsupported_rows"])


if __name__ == "__main__":
	unittest.main()
